"""Independent FITS/XISF encoders and complete synthetic camera projects."""
import hashlib
import json
import os
import signal
import sqlite3
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
import xml.etree.ElementTree as ET
import zlib

import lz4.block
import numpy as np
import zstandard
from astropy.io import fits

BINARY = str(Path(sys.argv.pop(1)).resolve())


def xisf(path, pixels, codec=None, shuffle=False, subblocks=False, normal=False, bad=False, bounds=None, auxiliary=False, keywords=None):
    formats = {"uint8": "UInt8", "uint16": "UInt16", "uint32": "UInt32", "float32": "Float32", "float64": "Float64"}
    p = np.asarray(pixels)
    channels = p.shape[0] if p.ndim == 3 else 1
    h, w = p.shape[-2:]
    raw = (p.transpose(1, 2, 0) if normal and p.ndim == 3 else p).astype(p.dtype.newbyteorder("<")).tobytes()
    if shuffle:
        raw = np.frombuffer(raw, dtype=np.uint8).reshape(-1, p.itemsize).T.tobytes()
    def compress(b):
        if codec in ("lz4", "lz4hc"):
            return lz4.block.compress(b, store_size=False, mode="high_compression" if codec == "lz4hc" else "default")
        if codec == "zlib":
            return zlib.compress(b)
        if codec == "zstd":
            return zstandard.ZstdCompressor().compress(b)
        return b
    chunks = [raw[:len(raw)//2], raw[len(raw)//2:]] if subblocks else [raw]
    encoded = [compress(c) for c in chunks]
    payload = b"".join(encoded)
    attrs = {"geometry": f"{w}:{h}:{channels}", "sampleFormat": formats[p.dtype.name],
             "colorSpace": "Gray" if channels == 1 else "RGB", "pixelStorage": "Normal" if normal else "Planar",
             "location": f"attachment:4096:{len(payload)}", "checksum": "sha256:" + ("0" * 64 if bad else hashlib.sha256(payload).hexdigest())}
    if codec:
        attrs["compression"] = codec + ("+sh" if shuffle else "") + f":{len(raw)}" + (f":{p.itemsize}" if shuffle else "")
    if subblocks:
        attrs["subblocks"] = ":".join(f"{len(e)},{len(r)}" for e, r in zip(encoded, chunks))
    if bounds is not None:
        attrs["bounds"] = bounds
    if auxiliary:
        attrs["id"] = "integration"
    root = ET.Element("xisf", version="1.0", xmlns="http://www.pixinsight.com/xisf")
    image = ET.SubElement(root, "Image", attrs)
    ET.SubElement(image, "FITSKeyword", name="OBJECT", value="'Independent fixture'")
    for key,value in (keywords or {}).items():
        literal = "'" + value.replace("'","''") + "'" if isinstance(value,str) else "T" if value is True else "F" if value is False else str(value)
        ET.SubElement(image,"FITSKeyword",name=key,value=literal)
    if auxiliary:
        aux=ET.SubElement(root,"Image",dict(attrs,id="rejection_low"))
        # Same payload is legal for this descriptor-only auxiliary fixture.
    xml = ET.tostring(root)
    path.write_bytes(b"XISF0100" + struct.pack("<II", len(xml), 0) + xml + bytes(4096 - 16 - len(xml)) + payload)


class Integration(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="stellastack-test-")
        self.root = Path(self.temp.name)
        self.env = dict(os.environ, XDG_CACHE_HOME=str(self.root / "cache"))

    def tearDown(self):
        self.temp.cleanup()

    def run_cli(self, *args, fail=False):
        p = subprocess.run([BINARY, *map(str, args)], env=self.env, text=True, capture_output=True, timeout=120)
        self.assertEqual(p.returncode != 0, fail, p.stdout + p.stderr)
        return p

    def test_explicit_calibration_and_metadata_matching(self):
        project, raw = self.camera_project()
        # Match the real PixInsight case: headers omit gain/temperature/acquisition fields.
        for p in list(raw.glob("dark*")) + list(raw.glob("bias*")):
            with fits.open(p, mode="update") as hdus:
                header = hdus[0].header
                kind = "masterDark" if "dark" in p.name else "masterBias"
                header["IMAGETYP"] = "Master Dark" if kind == "masterDark" else "Master Bias"
                for key in ("GAIN", "CCD-TEMP", "OFFSET", "READOUTM"):
                    header.pop(key, None)
                hdus[0].add_checksum()
            suffix = "__EXPOSURE_60.00s" if kind == "masterDark" else ""
            p.rename(raw / (kind + suffix + "__GAIN_100__TEMP_-10.0__" + p.stem + ".fits"))
        # Use one master of each kind rather than equally ranked duplicate masters.
        for kind in ("masterDark", "masterBias"):
            matches = sorted(raw.glob(kind + "*"))
            for p in matches[1:]:
                p.unlink()
        project = self.root / "inferred.stella"
        self.run_cli("init", project)
        self.run_cli("import", project, raw)
        error = self.run_cli("analyze", project, fail=True)
        self.assertIn("Calibrate first", error.stderr)
        assignments = json.loads(self.run_cli("calibration", project).stdout)
        self.assertTrue(all(len(row["dark"]) == 1 for row in assignments))
        inferred = json.loads(self.run_cli("list", project).stdout)
        dark = next(f for f in inferred if f["kind"] == "dark")
        self.assertEqual(dark["header"]["GAIN"], 100)
        self.assertEqual(dark["header"]["CCD-TEMP"], -10)
        self.assertEqual(dark["metadataSources"]["GAIN"], "filename")
        prepared = self.root / "prepared"
        events = self.run_cli("calibrate", project, prepared, "--json")
        self.assertIn('"event":"frame-updated"', events.stdout)
        frames = json.loads(self.run_cli("list", project).stdout)
        lights = [f for f in frames if f["kind"] == "light"]
        self.assertTrue(all(Path(f["calibratedPath"]).is_file() for f in lights))
        dates = {f["id"]: Path(f["calibratedPath"]).stat().st_mtime_ns for f in lights}
        self.run_cli("calibrate", project)
        self.assertEqual(dates, {f["id"]: Path(f["calibratedPath"]).stat().st_mtime_ns for f in lights})
        self.run_cli("analyze", project)
        reference = json.loads(self.run_cli("settings", project).stdout)["reference"]
        damaged = next(f for f in lights if f["id"] != reference)
        image = Path(damaged["calibratedPath"])
        image.write_bytes(b"corrupt prepared image")
        self.run_cli("analyze", project)
        # Corruption is reported per frame and makes stacking reject that input.
        rows = json.loads(self.run_cli("list", project).stdout)
        self.assertIn("Calibrate again", next(f for f in rows if f["id"] == damaged["id"])["error"])
        self.run_cli("calibrate", project)
        self.run_cli("analyze", project)
        # Known metadata conflicts must not be weakened by incomplete masters.
        self.run_cli("edit", project, "--ids", dark["id"], "--set", json.dumps({"header": {"GAIN": 200}}))
        plan = json.loads(self.run_cli("calibration", project).stdout)
        self.assertTrue(all(not row["dark"] for row in plan))

    def test_preparation_cancellation_and_publication_recovery(self):
        project, raw = self.camera_project(size=512)
        proc = subprocess.Popen([BINARY, "calibrate", str(project), "--json"], env=self.env,
                                text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        cancelled = False
        for line in proc.stdout:
            event = json.loads(line)
            if event.get("event") == "progress" and event.get("stage") == "calibrate" and event["done"] >= 1:
                proc.send_signal(signal.SIGINT)
                cancelled = True
                break
        stdout, stderr = proc.communicate(timeout=30)
        self.assertTrue(cancelled)
        self.assertEqual(proc.returncode, 130, stdout + stderr)
        rows = json.loads(self.run_cli("list", project).stdout)
        completed = [f for f in rows if f["kind"] == "light" and f["calibrationKey"]]
        self.assertGreaterEqual(len(completed), 1)
        self.assertLess(len(completed), 12)
        timestamps = {f["id"]: Path(f["calibratedPath"]).stat().st_mtime_ns for f in completed}
        self.run_cli("calibrate", project)
        rows = json.loads(self.run_cli("list", project).stdout)
        self.assertTrue(all(f["calibrationKey"] for f in rows if f["kind"] == "light"))
        for frame in completed:
            self.assertEqual(Path(frame["calibratedPath"]).stat().st_mtime_ns, timestamps[frame["id"]])
        recovered = completed[0]
        # Simulate interruption after atomic publication but before recording its prepared reference.
        with sqlite3.connect(project) as db:
            row = json.loads(db.execute("SELECT record FROM frames WHERE id=?", (recovered["id"],)).fetchone()[0])
            row["calibrationKey"] = ""
            row["calibratedPath"] = ""
            db.execute("UPDATE frames SET record=? WHERE id=?", (json.dumps(row), recovered["id"]))
        self.run_cli("calibrate", project)
        self.assertEqual(Path(recovered["calibratedPath"]).stat().st_mtime_ns, timestamps[recovered["id"]])
        # Recursive imports must not import these generated lights and calibrate them twice.
        count = len(rows)
        self.run_cli("import", project, Path(recovered["calibratedPath"]).parent)
        self.assertEqual(len(json.loads(self.run_cli("list", project).stdout)), count)
        missing = Path(recovered["calibratedPath"])
        missing.unlink()
        error = self.run_cli("analyze", project, fail=True)
        self.assertIn("Calibrate first", error.stderr)
        self.run_cli("calibrate", project)
        self.run_cli("analyze", project)

    def test_independent_xisf_matrix(self):
        for dtype in ("uint8", "uint16", "uint32", "float32", "float64"):
            pixels = np.arange(3*16*20, dtype=np.float64).reshape(3,16,20)
            if dtype.startswith("float"):
                pixels = (pixels - 100) / 7
                pixels[0,0,0] = np.nan
            pixels = pixels.astype(dtype)
            for codec in (None, "zlib", "lz4", "lz4hc", "zstd"):
                for shuffle in ((False, True) if codec else (False,)):
                    for normal in (False, True):
                        with self.subTest(dtype=dtype, codec=codec, shuffle=shuffle, normal=normal):
                            src = self.root / "input.xisf"
                            out = self.root / "converted.fits.fz"
                            xisf(src, pixels, codec, shuffle, subblocks=bool(codec), normal=normal)
                            self.run_cli("convert", src, out, "--format", "fits.fz", "--overwrite")
                            with fits.open(out, checksum=True) as hdus:
                                hdus.verify("exception")
                                np.testing.assert_array_equal(hdus[1].data, pixels.astype(np.float32))
                                self.assertEqual(hdus[1].header["OBJECT"], "Independent fixture")
                            # Checksums describe the physical binary table, not the virtual image HDU.
                            with fits.open(out, disable_image_compression=True) as physical:
                                for hdu in physical:
                                    self.assertEqual(hdu.verify_checksum(), 1)
                                    self.assertEqual(hdu.verify_datasum(), 1)

    def test_rice_and_corruption(self):
        pixels = np.arange(48*64, dtype=np.uint16).reshape(48,64)
        src = self.root / "rice.fits.fz"
        fits.HDUList([fits.PrimaryHDU(), fits.CompImageHDU(pixels, compression_type="RICE_1")]).writeto(src, checksum=True)
        out = self.root / "out.fits"
        self.run_cli("convert", src, out)
        np.testing.assert_array_equal(fits.getdata(out), pixels.astype(np.float32))
        bad = self.root / "bad.xisf"
        xisf(bad, pixels, "zstd", True, bad=True)
        error = self.run_cli("convert", bad, self.root / "must-not-exist.fits", fail=True)
        self.assertIn("checksum mismatch", error.stderr)
        self.assertFalse((self.root / "must-not-exist.fits").exists())

    def camera_project(self, cache=True, size=160):
        raw = self.root / "raw"
        raw.mkdir()
        rng = np.random.default_rng(420)
        h = w = size
        yy, xx = np.mgrid[:h,:w]
        flat = 0.85 + 0.3 * xx / (w-1)
        positions = [(x,y) for y in (26,61,98,134) for x in (23,58,94,132)]
        positions = [(x+rng.uniform(-3,3),y+rng.uniform(-3,3)) for x,y in positions]
        def write(name, data, kind, exposure=60, band="L", night="2026-09-20"):
            header = fits.Header({"IMAGETYP":kind,"EXPTIME":exposure,"FILTER":band,"DATE-OBS":night+"T22:10:00",
                                  "INSTRUME":"SyntheticCamera","GAIN":100,"OFFSET":10,"CCD-TEMP":-10,
                                  "XBINNING":1,"YBINNING":1,"ROWORDER":"TOP-DOWN"})
            fits.writeto(raw/name, data.astype(np.float32), header, checksum=True)
        for i in range(5):
            write(f"bias-{i}.fits", 80+rng.normal(0,.2,(h,w)), "Bias", 0)
            write(f"dark-{i}.fits", 120+rng.normal(0,.2,(h,w)), "Dark")
            write(f"flat-{i}.fits", 10000*flat+80+rng.normal(0,2,(h,w)), "Flat", 1)
        for i in range(12):
            base = np.full((h,w), 200.0)
            dx, dy = i%3-1, i%4-2
            for j,(x,y) in enumerate(positions):
                x,y=x+dx,y+dy
                if i%5==4:
                    x,y=w-1-x,h-1-y
                base += (1000+j*45)*np.exp(-((xx-x)**2+(yy-y)**2)/(2*1.8**2))
            if i==7:
                base[72,72]+=10000
            write(f"light-{i:02}.fits", (base+rng.normal(0,2,(h,w)))*flat+120, "Light", night="2026-09-20" if i<6 else "2026-09-21")
        project=self.root/"project.stella"
        self.run_cli("init",project)
        self.run_cli("import",project,raw)
        self.run_cli("settings",project,"--set",json.dumps({"memory":128*1024**2,"scratch":64*1024**2 if cache else 0,"threads":2}))
        return project, raw

    def test_complete_camera_project_and_cache_equivalence(self):
        project, raw = self.camera_project()
        self.run_cli("calibrate",project)
        self.run_cli("analyze",project)
        frames=json.loads(self.run_cli("list",project).stdout)
        lights=[f for f in frames if f["kind"]=="light"]
        self.assertEqual(len(lights),12)
        self.assertTrue(all(f["transform"]["valid"] for f in lights), [(f["path"],f["error"]) for f in lights])
        out=self.root/"masters"
        self.run_cli("stack",project,out)
        master=fits.getdata(out/"L-master.fits")
        self.assertEqual(master.shape,(160,160))
        self.assertTrue(np.isfinite(master[20:-20,20:-20]).all())
        self.assertLess(abs(float(np.nanmedian(master))-200),3)
        self.assertEqual(fits.getheader(out/"L-master.fits")["NCOMBINE"],12)
        self.assertGreater(np.nanmax(fits.getdata(out/"L-rejection.fits")),0)
        before=(out/"L-master.fits").read_bytes()
        self.run_cli("resume",project,out)
        self.assertEqual((out/"L-master.fits").read_bytes(),before)
        # The minimum RAM budget leaves no hot-frame cache after workspace
        # reservations, so this also checks cached versus streamed arithmetic.
        self.run_cli("settings",project,"--set",json.dumps({"memory":64*1024**2,"scratch":0,"threads":1}))
        # Force a cold serial analysis and compare the parallel catalogs/metrics.
        with sqlite3.connect(project) as db:
            for id_,record in db.execute("SELECT id,record FROM frames").fetchall():
                row=json.loads(record)
                if row["kind"]=="light":
                    row["analysisKey"]=""
                    row["calibrationKey"]=""
                    row["catalog"]=[]
                    db.execute("UPDATE frames SET record=? WHERE id=?",(json.dumps(row),id_))
        self.run_cli("calibrate",project)
        self.run_cli("analyze",project)
        serial=json.loads(self.run_cli("list",project).stdout)
        for parallel,row in zip(frames,serial):
            if row["kind"]=="light":
                self.assertEqual(row["metrics"],parallel["metrics"])
                self.assertEqual(row["transform"],parallel["transform"])
        streamed=self.root/"streamed"
        self.run_cli("stack",project,streamed)
        np.testing.assert_array_equal(fits.getdata(streamed/"L-master.fits"),master)
        for suffix in ("coverage", "weights", "rejection"):
            np.testing.assert_array_equal(fits.getdata(streamed/f"L-{suffix}.fits"),
                                          fits.getdata(out/f"L-{suffix}.fits"))
        export=self.root/"xisf"
        self.run_cli("export",project,export,"--format","xisf-zstd")
        self.run_cli("convert",export/"L-master.xisf",self.root/"roundtrip.fits")
        np.testing.assert_array_equal(fits.getdata(self.root/"roundtrip.fits"),master)
        changed=raw/"light-00.fits"
        with fits.open(changed,mode="update") as hdus:
            hdus[0].data[0,0]+=1
            hdus[0].add_checksum()
        error=self.run_cli("stack",project,self.root/"changed",fail=True)
        self.assertIn("Input changed",error.stderr)

    def test_osc_calibrated_before_debayer(self):
        project, raw = self.camera_project()
        # The luminance star field is sampled through an RGGB camera with a 2:1:0.5
        # color response. Calibration frames stay in the sensor domain.
        for path in raw.glob("*.fits"):
            with fits.open(path, mode="update") as hdus:
                hdu = hdus[0]
                hdu.header["BAYERPAT"] = "RGGB"
                hdu.header["FILTER"] = "OSC"
                if hdu.header["IMAGETYP"] == "Light":
                    signal_data = hdu.data - 120
                    signal_data[0::2,0::2] *= 2
                    signal_data[1::2,1::2] *= .5
                    hdu.data = signal_data + 120
                hdu.add_checksum()
        # Reimport after editing fixtures so descriptors match the immutable inputs.
        project.unlink()
        self.run_cli("init",project)
        self.run_cli("import",project,raw)
        self.run_cli("settings",project,"--set",json.dumps({"memory":128*1024**2,"scratch":64*1024**2,"threads":2}))
        self.run_cli("calibrate",project)
        self.run_cli("analyze",project)
        out=self.root/"osc"
        self.run_cli("stack",project,out)
        data=fits.getdata(out/"OSC-master.fits")
        self.assertEqual(data.shape,(3,160,160))
        levels=np.nanmedian(data[:,20:-20,20:-20],axis=(1,2))
        np.testing.assert_allclose(levels/levels[1],[2,1,.5],atol=.03)
        self.assertNotIn("BAYERPAT",fits.getheader(out/"OSC-master.fits"))
        self.run_cli("export",project,self.root/"osc-xisf","--format","xisf-zstd")
        self.run_cli("inspect",self.root/"osc-xisf/OSC-master.xisf")

    def test_all_mono_filters_share_reference_grid(self):
        project, raw = self.camera_project()
        for index, path in enumerate(sorted(raw.glob("light-*.fits"))):
            with fits.open(path,mode="update") as hdus:
                hdus[0].header["FILTER"] = "LRGBSHO"[index%7]
                hdus[0].add_checksum()
        # Seven separate flat groups; bias and darks remain shared.
        for band in "RGBSHO":
            for path in list(raw.glob("flat-*.fits")):
                with fits.open(path) as hdus:
                    hdus[0].header["FILTER"] = band
                    hdus.writeto(raw/(band+"-"+path.name),checksum=True)
        project.unlink()
        self.run_cli("init",project)
        self.run_cli("import",project,raw)
        self.run_cli("settings",project,"--set",json.dumps({"memory":128*1024**2,"scratch":64*1024**2,"threads":2}))
        self.run_cli("calibrate",project)
        self.run_cli("analyze",project)
        out=self.root/"mono"
        self.run_cli("stack",project,out)
        maxima=[]
        for band in "LRGBSHO":
            data=fits.getdata(out/(band+"-master.fits"))
            self.assertEqual(data.shape,(160,160))
            # Compare the brightest real star, excluding the injected cosmic ray:
            # small filter groups intentionally do not apply statistical rejection.
            patch=data[110:150,110:150]
            maxima.append(np.unravel_index(np.nanargmax(patch),patch.shape))
            self.assertEqual(np.nanmax(fits.getdata(out/(band+"-rejection.fits"))),0)
        self.assertEqual(len(set(maxima)),1,maxima)

    def test_cancellation_and_durable_resume(self):
        project, raw = self.camera_project(size=512)
        self.run_cli("settings",project,"--set",json.dumps({"memory":64*1024**2}))
        self.run_cli("calibrate",project)
        self.run_cli("analyze",project)
        out=self.root/"interrupted"
        proc=subprocess.Popen([BINARY,"stack",str(project),str(out),"--json"],env=self.env,
                              text=True,stdout=subprocess.PIPE,stderr=subprocess.PIPE)
        cancelled=False
        for line in proc.stdout:
            event=json.loads(line)
            if event.get("stage")=="checkpoint":
                proc.send_signal(signal.SIGINT)
                cancelled=True
                break
        stdout,stderr=proc.communicate(timeout=120)
        self.assertTrue(cancelled,stdout+stderr)
        self.assertEqual(proc.returncode,130,stdout+stderr)
        self.assertTrue(list(out.glob(".stellastack-*/*.band")))
        resumed=self.run_cli("resume",project,out,"--json")
        self.assertIn("Restored verified band",resumed.stdout)
        reference=self.root/"uninterrupted"
        self.run_cli("stack",project,reference)
        np.testing.assert_array_equal(fits.getdata(out/"L-master.fits"),fits.getdata(reference/"L-master.fits"))
        self.assertFalse(list(out.glob(".stellastack-*")))

    def test_partial_publication_and_corrupt_cache(self):
        project, raw = self.camera_project()
        self.run_cli("calibrate",project)
        self.run_cli("analyze",project)
        settings=json.loads(self.run_cli("settings",project).stdout)
        cached=list(Path(settings["cacheDirectory"]).glob("cache-*.fits"))
        self.assertTrue(cached)
        cached[0].write_bytes(b"damaged derived cache")
        out=self.root/"publication"
        self.run_cli("stack",project,out)
        # Reconstruct a crash after the manifest and the first atomic publication:
        # master already published, the three maps still staged, result uncommitted.
        with sqlite3.connect(project) as db:
            result=json.loads(db.execute("SELECT value FROM records WHERE key='result:L'").fetchone()[0])
            for row in result["files"][1:]:
                staged=Path(row["staged"])
                staged.parent.mkdir(exist_ok=True)
                Path(row["path"]).rename(staged)
            db.execute("UPDATE records SET value=? WHERE key='pending:L'",(json.dumps(result),))
            db.execute("DELETE FROM records WHERE key IN ('result:L','results')")
        before=(out/"L-master.fits").read_bytes()
        resumed=self.run_cli("resume",project,out,"--json")
        self.assertIn("Recovered L output publication",resumed.stdout)
        self.assertEqual((out/"L-master.fits").read_bytes(),before)
        self.assertEqual(len(list(out.glob("*.fits"))),4)
        self.assertFalse(list(out.glob(".stellastack-*")))

    def test_fits_long_strings_offsets_and_scaling(self):
        src=self.root/"long.fits"
        image=np.arange(32*32,dtype=np.int16).reshape(32,32)
        header=fits.Header({"OBJECT":"long string "*30,"BAYERPAT":"RGGB","XBAYROFF":1,"YBAYROFF":1})
        hdu=fits.PrimaryHDU(image,header)
        hdu.scale("int16",bscale=2,bzero=100)
        hdu.writeto(src,checksum=True)
        out=self.root/"scaled.xisf"
        self.run_cli("convert",src,out,"--format","xisf-zstd")
        inspected=json.loads(self.run_cli("inspect",out).stdout)
        self.assertEqual(inspected["cfa"],"BGGR")
        self.assertEqual(inspected["header"]["OBJECT"],"long string "*29+"long string")
        self.run_cli("convert",out,self.root/"round.fits")
        np.testing.assert_array_equal(fits.getdata(self.root/"round.fits"),fits.getdata(src))

    def test_bias_subtracted_dark_master_and_dark_flats(self):
        project, raw = self.camera_project()
        frames=json.loads(self.run_cli("list",project).stdout)
        dark_ids=",".join(str(f["id"]) for f in frames if f["kind"]=="dark")
        self.run_cli("edit",project,"--ids",dark_ids,"--set",'{"selection":-1}')
        header=fits.getheader(raw/"dark-0.fits")
        header["IMAGETYP"]="Master Dark"
        header["BIASSUB"]=True
        fits.writeto(raw/"master-dark.fits",np.full((160,160),40,dtype=np.float32),header,checksum=True)
        for i in range(5):
            header=fits.getheader(raw/f"flat-{i}.fits")
            header["IMAGETYP"]="Dark Flat"
            fits.writeto(raw/f"darkflat-{i}.fits",np.full((160,160),80,dtype=np.float32),header,checksum=True)
        self.run_cli("import",project,raw)
        self.run_cli("calibrate",project)
        self.run_cli("analyze",project)
        out=self.root/"calibrated"
        self.run_cli("stack",project,out)
        self.assertLess(abs(float(np.nanmedian(fits.getdata(out/"L-master.fits")))-200),3)

    def test_edit_validation_is_transactional(self):
        project,raw=self.camera_project()
        before=json.loads(self.run_cli("list",project).stdout)
        id_=before[0]["id"]
        self.run_cli("edit",project,"--ids",f"{id_},999999","--set",'{"selection":-1}',fail=True)
        self.assertEqual(json.loads(self.run_cli("list",project).stdout),before)
        self.run_cli("edit",project,"--ids",str(id_),"--set",'{"kind":"flta"}',fail=True)
        self.run_cli("edit",project,"--ids",str(id_),"--set",'{"selection":2}',fail=True)
        self.run_cli("edit",project,"--ids",str(id_),"--set",'{"header":{"GAIN":200}}')
        changed=json.loads(self.run_cli("list",project).stdout)[0]
        self.assertEqual(changed["header"]["GAIN"],200)
        self.assertEqual(changed["header"]["INSTRUME"],"SyntheticCamera")

    def test_output_write_failure_preserves_sources_and_resumes(self):
        import resource
        project,raw=self.camera_project(size=512)
        self.run_cli("settings",project,"--set",json.dumps({"memory":64*1024**2,"scratch":0}))
        self.run_cli("calibrate",project)
        self.run_cli("analyze",project)
        source=raw/"light-00.fits"
        before=hashlib.sha256(source.read_bytes()).hexdigest()
        def limit():
            signal.signal(signal.SIGXFSZ,signal.SIG_IGN)
            resource.setrlimit(resource.RLIMIT_FSIZE,(400*1024,400*1024))
        out=self.root/"disk-failure"
        result=subprocess.run([BINARY,"stack",str(project),str(out)],env=self.env,text=True,
                              capture_output=True,timeout=120,preexec_fn=limit)
        self.assertNotEqual(result.returncode,0,result.stderr)
        self.assertFalse((out/"L-master.fits").exists())
        self.assertEqual(hashlib.sha256(source.read_bytes()).hexdigest(),before)
        self.run_cli("resume",project,out)
        self.assertEqual(fits.getdata(out/"L-master.fits").shape,(512,512))

    def test_standalone_flat_masters_user_selects_bias_or_darkflat(self):
        project,raw=self.camera_project()
        frames=json.loads(self.run_cli("list",project).stdout)
        flat_ids=",".join(str(f["id"]) for f in frames if f["kind"]=="flat")
        bias_out=self.root/"bias-flats"
        self.run_cli("masters",project,bias_out,"--ids",flat_ids,"--flat-calibration","bias")
        bias_path=next(bias_out.glob("flat-*-master.fits"))
        bias_master=fits.getdata(bias_path)
        self.assertLess(abs(float(np.nanmedian(bias_master))-1),1e-5)
        np.testing.assert_allclose(np.nanmedian(bias_master,axis=0),np.linspace(.85,1.15,160),atol=.001)
        self.assertEqual(fits.getheader(bias_path)["NCOMBINE"],5)
        self.assertEqual(fits.getheader(bias_path)["CALMETH"],"bias")
        self.run_cli("masters",project,self.root/"missing-df","--ids",flat_ids,
                     "--flat-calibration","darkflat",fail=True)
        # Different DF signal makes the choice observable: DF subtracts 1080,
        # while bias subtracts 80, and both normalize to unity.
        header=fits.getheader(raw/"flat-0.fits")
        header["IMAGETYP"]="Dark Flat"
        for i in range(5):
            fits.writeto(raw/f"darkflat-{i}.fits",np.full((160,160),1080,dtype=np.float32),header,checksum=True)
        self.run_cli("import",project,raw)
        dark_out=self.root/"dark-flats"
        self.run_cli("masters",project,dark_out,"--ids",flat_ids,"--flat-calibration","darkflat")
        dark_path=next(dark_out.glob("flat-*-master.fits"))
        dark_master=fits.getdata(dark_path)
        np.testing.assert_allclose(np.nanmedian(dark_master,axis=0),np.linspace(7500/9000,10500/9000,160),atol=.001)
        self.assertEqual(fits.getheader(dark_path)["CALMETH"],"darkflat")
        still_bias=self.root/"bias-choice-with-df-present"
        self.run_cli("masters",project,still_bias,"--ids",flat_ids,"--flat-calibration","bias")
        np.testing.assert_array_equal(fits.getdata(next(still_bias.glob("flat-*-master.fits"))),bias_master)
        # Calibration-only projects can build masters without any lights or analysis.
        standalone=self.root/"flat-only.stella"
        self.run_cli("init",standalone)
        self.run_cli("import",standalone,*sorted(raw.glob("flat-*.fits")),*sorted(raw.glob("bias-*.fits")))
        self.run_cli("masters",standalone,self.root/"standalone","--flat-calibration","bias","--format","xisf-zstd")
        self.assertEqual(len(list((self.root/"standalone").glob("*.xisf"))),2)

    def test_pixinsight_normalized_masters_with_integer_xisf_lights(self):
        project,raw=self.camera_project()
        project.unlink()
        for path in list(raw.glob("*.fits")):
            kind=fits.getheader(path)["IMAGETYP"]
            if kind in ("Bias","Flat","Dark"):
                path.unlink()
                continue
            data=fits.getdata(path).round().clip(0,65535).astype(np.uint16)
            header=fits.getheader(path)
            keys={key:header[key] for key in ("IMAGETYP","INSTRUME","GAIN","OFFSET","ROWORDER","CCD-TEMP","EXPTIME","FILTER","DATE-OBS","XBINNING","YBINNING")}
            xisf(path.with_suffix(".xisf"),data,"lz4hc",True,keywords=keys)
            path.unlink()
        yy,xx=np.mgrid[:160,:160]
        flat=.85+.3*xx/159
        keys={"IMAGETYP":"Master Dark","INSTRUME":"SyntheticCamera","GAIN":100,"OFFSET":10,"ROWORDER":"TOP-DOWN","CCD-TEMP":-10,"EXPTIME":60,"XBINNING":1,"YBINNING":1,"FILTER":"L","BIASSUB":False}
        xisf(raw/"dark.xisf",np.full((160,160),120/65535,dtype=np.float32),bounds="0:1",auxiliary=True,keywords=keys)
        keys["IMAGETYP"]="Master Flat"
        keys["EXPTIME"]=1
        xisf(raw/"flat.xisf",flat.astype(np.float32),bounds="0:1",auxiliary=True,keywords=keys)
        self.run_cli("init",project)
        self.run_cli("import",project,raw)
        self.run_cli("settings",project,"--set",json.dumps({"memory":128*1024**2,"scratch":64*1024**2,"threads":2}))
        self.run_cli("calibrate",project)
        self.run_cli("analyze",project)
        out=self.root/"mixed-units"
        self.run_cli("stack",project,out)
        master=fits.getdata(out/"L-master.fits")
        self.assertLess(abs(float(np.nanmedian(master))*65535-200),3)
        self.assertEqual(fits.getheader(out/"L-master.fits")["BUNIT"],"relative")
        self.run_cli("export",project,self.root/"mixed-xisf","--format","xisf-zstd")
        self.run_cli("inspect",self.root/"mixed-xisf/L-master.xisf")
        # Converted integer files retain physical values and their nominal scale.
        self.run_cli("convert",raw/"light-00.xisf",self.root/"integer.fits")
        self.assertAlmostEqual(fits.getheader(self.root/"integer.fits")["SAMPLESCL"],1/65535)
        # Unknown additional images are never silently selected.
        ambiguous=raw/"flat.xisf"
        payload=ambiguous.read_bytes().replace(b'id="rejection_low"',b'id="another_image"')
        # Descriptor has equal byte length, preserving offsets and header length.
        ambiguous.write_bytes(payload)
        self.run_cli("inspect",ambiguous,fail=True)


if __name__ == "__main__":
    unittest.main()
