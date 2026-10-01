#include "converter.hpp"
#include "siderastack/core.hpp"
#include <QCryptographicHash>
#include <QFile>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QXmlStreamWriter>
#include <algorithm>
#include <bit>
#include <cstring>
#include <fitsio.h>
#include <fstream>
#include <mutex>
#include <openssl/evp.h>
#include <set>
#include <zstd.h>

namespace ss {
void checkpoint() {
    x2f::checkpoint();
}
void cancel() {
    x2f::interrupted = 1;
}
void clearCancellation() {
    x2f::interrupted = 0;
}
QByteArray json(const QJsonObject &o) {
    return QJsonDocument(o).toJson(QJsonDocument::Compact);
}
QJsonObject parseJson(const QByteArray &bytes) {
    QJsonParseError error;
    auto d = QJsonDocument::fromJson(bytes, &error);
    if (error.error != QJsonParseError::NoError || !d.isObject())
        throw Error("Invalid project JSON: " + error.errorString().toStdString());
    return d.object();
}
std::string str(const QJsonObject &o, const char *key, const std::string &fallback) {
    const auto v = o.value(QLatin1String(key));
    if (v.isString())
        return v.toString().toStdString();
    if (v.isDouble())
        return QString::number(v.toDouble(), 'g', 17).toStdString();
    if (v.isBool())
        return v.toBool() ? "T" : "F";
    return fallback;
}
double number(const QJsonObject &o, const char *key, double fallback) {
    auto v = o.value(QLatin1String(key));
    if (v.isDouble())
        return v.toDouble();
    if (v.isString()) {
        bool ok = false;
        auto n = v.toString().toDouble(&ok);
        if (ok && std::isfinite(n))
            return n;
    }
    return fallback;
}
void put(QJsonObject &o, const char *key, double value) {
    o[QLatin1String(key)] = std::isfinite(value) ? QJsonValue(value) : QJsonValue();
}
std::string hash(const std::string &s) {
    return QCryptographicHash::hash(QByteArray::fromStdString(s), QCryptographicHash::Sha256)
        .toHex()
        .toStdString();
}
namespace {
using Digest = std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)>;
Digest newDigest() {
    Digest context(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    if (!context || EVP_DigestInit_ex(context.get(), EVP_sha256(), nullptr) != 1)
        throw Error("Cannot initialize SHA-256");
    return context;
}
QByteArray finishDigest(EVP_MD_CTX *context) {
    unsigned char out[EVP_MAX_MD_SIZE];
    unsigned int length = 0;
    if (EVP_DigestFinal_ex(context, out, &length) != 1)
        throw Error("Cannot finalize SHA-256");
    return QByteArray(reinterpret_cast<const char *>(out), length).toHex();
}
} // namespace
QByteArray sha256(const QByteArray &data) {
    auto context = newDigest();
    if (EVP_DigestUpdate(context.get(), data.constData(), size_t(data.size())) != 1)
        throw Error("Cannot calculate SHA-256");
    return finishDigest(context.get());
}
std::string fingerprint(const fs::path &path) {
    QFile file(QString::fromStdString(path.string()));
    if (!file.open(QIODevice::ReadOnly))
        throw Error("Cannot fingerprint " + path.string());
    auto context = newDigest();
    while (!file.atEnd()) {
        checkpoint();
        auto block = file.read(4 * MiB);
        if (block.isEmpty() && file.error() != QFile::NoError)
            throw Error("Read failed: " + path.string());
        if (EVP_DigestUpdate(context.get(), block.constData(), size_t(block.size())) != 1)
            throw Error("Cannot calculate source SHA-256");
    }
    return finishDigest(context.get()).toStdString();
}
Format parseFormat(const std::string &s) {
    if (s == "fits")
        return Format::Fits;
    if (s == "fits.fz")
        return Format::FitsCompressed;
    if (s == "xisf")
        return Format::Xisf;
    if (s == "xisf-zstd")
        return Format::XisfCompressed;
    throw Error("Format must be fits, fits.fz, xisf, or xisf-zstd");
}
std::string extension(Format f) {
    return f == Format::Fits ? ".fits" : f == Format::FitsCompressed ? ".fits.fz" : ".xisf";
}
namespace {
std::recursive_mutex fitsMutex;
struct Fits {
    std::unique_lock<std::recursive_mutex> lock{fitsMutex, std::defer_lock};
    fitsfile *p = nullptr;
    Fits() {
        if (!fits_is_reentrant())
            lock.lock();
    }
    ~Fits() {
        if (p) {
            int s = 0;
            fits_close_file(p, &s);
        }
    }
    void close() {
        int s = 0;
        fits_close_file(p, &s);
        p = nullptr;
        x2f::checkFits(s, "close FITS");
    }
};
void addCard(QJsonObject &h, const x2f::Card &c) {
    if (c.name.empty())
        return;
    QJsonValue v(QString::fromStdString(c.value));
    if (c.type == x2f::ValueType::Logical)
        v = c.value == "T";
    if (c.type == x2f::ValueType::Integer || c.type == x2f::ValueType::Real) {
        auto value = QString::fromStdString(c.value).replace('D', 'E');
        bool ok = false;
        double n = value.toDouble(&ok);
        if (ok && std::isfinite(n))
            v = n;
    }
    const auto key = QString::fromStdString(c.name);
    if (c.type == x2f::ValueType::Commentary) {
        const auto previous = h.value(key).toString();
        h[key] = previous.isEmpty() ? v : QJsonValue(previous + '\n' + v.toString());
    } else if (h.contains(key) && h[key] != v)
        throw Error("Conflicting metadata: " + c.name);
    else
        h[key] = v;
}
void validateGeometry(const Image &im, uint64_t budget, bool headerOnly) {
    if (im.width <= 0 || im.height <= 0 || (im.channels != 1 && im.channels != 3))
        throw Error("Expected a 2D mono or RGB image");
    if (uint64_t(im.width) * uint64_t(im.height) > uint64_t(SIZE_MAX) / 4 / im.channels)
        throw Error("Image dimensions overflow");
    if (!headerOnly && im.samples() > budget / sizeof(float))
        throw Error("Image exceeds memory budget");
}
void normalizeCfa(Image &im, const std::string &xmlCfa = {}) {
    auto cfa = x2f::upper(x2f::trim(str(im.header, "BAYERPAT")));
    auto offset = [&](const char *key, const char *alias) {
        double a = number(im.header, key, number(im.header, alias, 0));
        double b = number(im.header, alias, a);
        if (!std::isfinite(a) || a != std::trunc(a) || std::abs(a) > INT_MAX || a != b)
            throw Error("Invalid or conflicting CFA offsets");
        return int(a) & 1;
    };
    const int x = offset("XBAYROFF", "XBAYEROFF"), y = offset("YBAYROFF", "YBAYEROFF");
    const std::set<std::string> valid{"RGGB", "BGGR", "GBRG", "GRBG"};
    if (!cfa.empty()) {
        if (!valid.contains(cfa) || im.channels != 1)
            throw Error("Unsupported Bayer description");
        auto old = cfa;
        for (int j = 0; j < 2; j++)
            for (int i = 0; i < 2; i++)
                cfa[j * 2 + i] = old[((j + y) & 1) * 2 + ((i + x) & 1)];
    }
    if (!xmlCfa.empty()) {
        if (!valid.contains(xmlCfa) || (!cfa.empty() && cfa != xmlCfa) || (cfa.empty() && (x || y)))
            throw Error("Conflicting XISF/FITS CFA metadata");
        cfa = xmlCfa;
    }
    im.cfa = cfa;
    if (!cfa.empty()) {
        im.header["BAYERPAT"] = QString::fromStdString(cfa);
        im.header["XBAYROFF"] = 0;
        im.header["YBAYROFF"] = 0;
        im.header.remove("XBAYEROFF");
        im.header.remove("YBAYEROFF");
    }
}
std::string fitsLiteral(const QJsonValue &v) {
    if (v.isBool())
        return v.toBool() ? "T" : "F";
    if (v.isDouble())
        return QString::number(v.toDouble(), 'g', 17).toStdString();
    return "'" + v.toString().replace("'", "''").toStdString() + "'";
}
void writeFits(const fs::path &path, const Image &im, bool compressed) {
    Fits f;
    int s = 0;
    fits_create_diskfile(&f.p, path.c_str(), &s);
    if (compressed) {
        fits_create_img(f.p, BYTE_IMG, 0, nullptr, &s);
        fits_set_compression_type(f.p, GZIP_2, &s);
        fits_set_quantize_level(f.p, 0, &s);
    }
    long axes[]{im.width, im.height, im.channels};
    fits_create_img(f.p, FLOAT_IMG, im.channels == 1 ? 2 : 3, axes, &s);
    x2f::checkFits(s, "create FITS image");
    x2f::Metadata meta;
    for (auto it = im.header.begin(); it != im.header.end(); ++it) {
        auto key = it.key().toStdString();
        if (x2f::structuralKeyword(key) || key == "BAYERPAT" || key == "XBAYROFF" || key == "YBAYROFF" ||
            key == "ROWORDER" || key == "CONTINUE" || key == "SAMPLESCL" ||
            (key == "BUNIT" && im.normalized && im.pixelScale == 1))
            continue;
        auto card = x2f::parseKeyword({key, fitsLiteral(it.value()), {}});
        if (key == "HISTORY" || key == "COMMENT") {
            card.type = x2f::ValueType::Commentary;
            card.value = it.value().toString().toStdString();
        }
        bool changed = false;
        card.value = x2f::ascii(card.value, changed);
        meta.cards.push_back(std::move(card));
    }
    if (im.normalized && im.pixelScale != 1)
        meta.cards.push_back({"SAMPLESCL", QString::number(im.pixelScale, 'g', 17).toUpper().toStdString(),
                              "Scale to relative units", x2f::ValueType::Real});
    if (im.normalized && im.pixelScale == 1)
        meta.cards.push_back({"BUNIT", "relative", {}, x2f::ValueType::String});
    if (!im.cfa.empty())
        meta.cards.push_back({"BAYERPAT", im.cfa, {}, x2f::ValueType::String});
    meta.cards.push_back({"ROWORDER", str(im.header, "ROWORDER", "TOP-DOWN"), {}, x2f::ValueType::String});
    for (auto &record : x2f::headerRecords(meta))
        fits_write_record(f.p, record.c_str(), &s);
    constexpr size_t chunk = 2 * MiB / sizeof(float);
    for (size_t i = 0; i < im.samples(); i += chunk) {
        checkpoint();
        fits_write_img(f.p, TFLOAT, LONGLONG(i + 1), std::min(chunk, im.samples() - i),
                       const_cast<float *>(im.pixels.data() + i), &s);
        x2f::checkFits(s, "write FITS pixels");
    }
    fits_write_chksum(f.p, &s);
    if (compressed) {
        int t = 0;
        fits_movabs_hdu(f.p, 1, &t, &s);
        fits_write_chksum(f.p, &s);
    }
    x2f::checkFits(s, "write FITS checksums");
    f.close();
}
void writeXisf(const fs::path &path, const Image &im, bool compressed) {
    std::vector<unsigned char> bytes(im.samples() * 4);
    for (size_t i = 0; i < im.samples(); ++i) {
        auto bits = std::bit_cast<uint32_t>(im.pixels[i]);
        for (size_t b = 0; b < 4; ++b)
            bytes[compressed ? b * im.samples() + i : 4 * i + b] =
                static_cast<unsigned char>(bits >> (8 * b));
    }
    const auto rawSize = bytes.size();
    if (compressed) {
        std::vector<unsigned char> out(ZSTD_compressBound(bytes.size()));
        auto n = ZSTD_compress(out.data(), out.size(), bytes.data(), bytes.size(), 3);
        if (ZSTD_isError(n))
            throw Error(ZSTD_getErrorName(n));
        out.resize(n);
        bytes = std::move(out);
    }
    auto checksum = sha256(
        QByteArray::fromRawData(reinterpret_cast<const char *>(bytes.data()), qsizetype(bytes.size())));
    QByteArray xml;
    size_t offset = 4096;
    for (;;) {
        xml.clear();
        QXmlStreamWriter w(&xml);
        w.writeStartDocument();
        w.writeStartElement("xisf");
        w.writeDefaultNamespace("http://www.pixinsight.com/xisf");
        w.writeAttribute("version", "1.0");
        w.writeStartElement("Image");
        w.writeAttribute("geometry", QString("%1:%2:%3").arg(im.width).arg(im.height).arg(im.channels));
        w.writeAttribute("sampleFormat", "Float32");
        if (im.normalized)
            w.writeAttribute("bounds", "0:" + QString::number(1 / im.pixelScale, 'g', 17));
        w.writeAttribute("colorSpace", im.channels == 1 ? "Gray" : "RGB");
        w.writeAttribute("pixelStorage", "Planar");
        w.writeAttribute("location", QString("attachment:%1:%2").arg(offset).arg(bytes.size()));
        w.writeAttribute("checksum", "sha256:" + QString::fromLatin1(checksum));
        if (compressed)
            w.writeAttribute("compression", QString("zstd+sh:%1:4").arg(rawSize));
        for (auto it = im.header.begin(); it != im.header.end(); ++it) {
            if (x2f::structuralKeyword(it.key().toStdString()))
                continue;
            w.writeEmptyElement("FITSKeyword");
            w.writeAttribute("name", it.key());
            w.writeAttribute("value", QString::fromStdString(fitsLiteral(it.value())));
        }
        if (!im.cfa.empty()) {
            w.writeEmptyElement("ColorFilterArray");
            w.writeAttribute("width", "2");
            w.writeAttribute("height", "2");
            w.writeAttribute("pattern", QString::fromStdString(im.cfa));
        }
        w.writeEndElement();
        w.writeEndElement();
        w.writeEndDocument();
        if (size_t(xml.size()) + 16 <= offset)
            break;
        offset = ((size_t(xml.size()) + 16 + 4095) / 4096) * 4096;
    }
    if (xml.size() > 64 * 1024 * 1024)
        throw Error("Output XISF header exceeds 64 MiB");
    std::ofstream out(path, std::ios::binary);
    out.exceptions(std::ios::badbit | std::ios::failbit);
    out.write("XISF0100", 8);
    uint32_t n = uint32_t(xml.size());
    char length[8]{};
    for (int i = 0; i < 4; ++i)
        length[i] = char(n >> (8 * i));
    out.write(length, 8);
    out.write(xml.data(), xml.size());
    std::string padding(offset - 16 - size_t(xml.size()), '\0');
    out.write(padding.data(), std::streamsize(padding.size()));
    for (size_t i = 0; i < bytes.size(); i += MiB) {
        checkpoint();
        out.write(reinterpret_cast<const char *>(bytes.data() + i),
                  std::streamsize(std::min<uint64_t>(MiB, bytes.size() - i)));
    }
    out.close();
}
} // namespace
void normalizeMetadata(Image &im) {
    normalizeCfa(im);
    if (im.header.contains("SS_SAMPLE_SCALE") || im.header.contains("SAMPLESCL")) {
        im.pixelScale = number(im.header, "SS_SAMPLE_SCALE", number(im.header, "SAMPLESCL"));
        if (!std::isfinite(im.pixelScale) || im.pixelScale <= 0)
            throw Error("Sample scale must be positive and finite");
        im.normalized = true;
    }
}
Image readImage(const fs::path &path, bool headerOnly, uint64_t budget) {
    checkpoint();
    Image im;
    if (x2f::upper(path.extension().string()) == ".XISF") {
        auto source = x2f::readSource(path, headerOnly, budget);
        if (source.width > INT_MAX || source.height > INT_MAX)
            throw Error("Image dimensions exceed supported coordinates");
        im.width = int(source.width);
        im.height = int(source.height);
        im.channels = int(source.channels);
        im.pixelScale = source.pixelScale;
        im.normalized = source.normalized;
        im.pixels = std::move(source.floatPixels);
        auto metadata = x2f::prepareMetadata(source, false);
        for (const auto &c : metadata.cards)
            addCard(im.header, c);
        // prepareMetadata already normalizes CFA offsets, so do not apply them twice.
        normalizeCfa(im, source.cfa);
    } else {
        Fits f;
        int s = 0, count = 0, chosen = 0;
        fits_open_diskfile(&f.p, fs::absolute(path).c_str(), READONLY, &s);
        x2f::checkFits(s, "open FITS");
        fits_get_num_hdus(f.p, &count, &s);
        for (int i = 1; i <= count; ++i) {
            int type = 0, naxis = 0;
            fits_movabs_hdu(f.p, i, &type, &s);
            if (type != IMAGE_HDU && !fits_is_compressed_image(f.p, &s))
                continue;
            fits_get_img_dim(f.p, &naxis, &s);
            if (!naxis)
                continue;
            if (chosen)
                throw Error("Multiple FITS images: extract the intended image before import");
            chosen = i;
        }
        if (!chosen)
            throw Error("FITS contains no image");
        int type = 0, naxis = 0;
        long axes[3]{};
        fits_movabs_hdu(f.p, chosen, &type, &s);
        fits_get_img_dim(f.p, &naxis, &s);
        fits_get_img_size(f.p, 3, axes, &s);
        x2f::checkFits(s, "read FITS geometry");
        if (naxis < 2 || naxis > 3 || axes[0] > INT_MAX || axes[1] > INT_MAX || (naxis == 3 && axes[2] != 3))
            throw Error("Unsupported FITS dimensions");
        im.width = int(axes[0]);
        im.height = int(axes[1]);
        im.channels = naxis == 3 ? 3 : 1;
        validateGeometry(im, budget, headerOnly);
        int keys = 0, more = 0;
        fits_get_hdrspace(f.p, &keys, &more, &s);
        for (int i = 1; i <= keys; ++i) {
            char k[FLEN_KEYWORD]{}, v[FLEN_VALUE]{}, c[FLEN_COMMENT]{};
            fits_read_keyn(f.p, i, k, v, c, &s);
            if (std::string(k) == "CONTINUE")
                continue; // CFITSIO reconstructs the preceding long string below.
            auto card = x2f::parseKeyword({k, v, c});
            if (card.type == x2f::ValueType::String) {
                char *longValue = nullptr;
                fits_read_key_longstr(f.p, k, &longValue, nullptr, &s);
                x2f::checkFits(s, "read FITS string");
                card.value = longValue ? longValue : "";
                fits_free_memory(longValue, &s);
                // FITS string padding is insignificant, including FILTER padding.
                while (!card.value.empty() && card.value.back() == ' ')
                    card.value.pop_back();
            }
            if (std::string(k) == "HISTORY" || std::string(k) == "COMMENT") {
                card.type = x2f::ValueType::Commentary;
                card.value = c;
            }
            addCard(im.header, card);
        }
        const auto bitpix = number(im.header, "BITPIX", number(im.header, "ZBITPIX", -32));
        const double zero = number(im.header, "BZERO", 0), scale = number(im.header, "BSCALE", 1);
        if (bitpix == 8 || ((bitpix == 16 || bitpix == 32) && zero == std::pow(2.0, bitpix - 1))) {
            im.pixelScale = 1 / (scale * (std::pow(2.0, bitpix - 1) - 1) + zero);
            im.normalized = true;
        } else if (str(im.header, "BUNIT") == "relative")
            im.normalized = true;
        normalizeMetadata(im);
        if (!headerOnly) {
            im.pixels.resize(im.samples());
            float null = std::numeric_limits<float>::quiet_NaN();
            int any = 0;
            constexpr size_t chunk = 2 * MiB / sizeof(float);
            for (size_t i = 0; i < im.samples(); i += chunk) {
                checkpoint();
                fits_read_img(f.p, TFLOAT, LONGLONG(i + 1), std::min(chunk, im.samples() - i), &null,
                              im.pixels.data() + i, &any, &s);
                x2f::checkFits(s, "read FITS pixels");
            }
            for (int i = 1; i <= count; ++i) {
                int data = 0, head = 0;
                fits_movabs_hdu(f.p, i, &type, &s);
                fits_verify_chksum(f.p, &data, &head, &s);
                x2f::checkFits(s, "verify FITS checksum");
                if (data < 0 || head < 0)
                    throw Error("FITS checksum mismatch");
            }
        }
        f.close();
    }
    normalizeMetadata(im);
    validateGeometry(im, budget, headerOnly);
    return im;
}
void writeImage(const fs::path &path, const Image &im, Format format, bool overwrite) {
    validateGeometry(im, UINT64_MAX, false);
    if (im.pixels.size() != im.samples())
        throw Error("Incomplete image buffer");
    x2f::TemporaryOutput temp(path);
    // Preserve the real extension so independent readback selects the correct decoder.
    auto destination = temp.file.parent_path() / ("master" + extension(format));
    try {
        if (format == Format::Fits || format == Format::FitsCompressed)
            writeFits(destination, im, format == Format::FitsCompressed);
        else
            writeXisf(destination, im, format == Format::XisfCompressed);
        auto check =
            readImage(destination, false, std::max<uint64_t>(64 * MiB, im.samples() * sizeof(float) * 16));
        if (check.width != im.width || check.height != im.height || check.channels != im.channels)
            throw Error("Output geometry verification failed");
        for (size_t i = 0; i < im.samples(); ++i)
            if (im.pixels[i] != check.pixels[i] && !(std::isnan(im.pixels[i]) && std::isnan(check.pixels[i])))
                throw Error("Output pixel verification failed");
        x2f::publish(destination, path, overwrite);
    } catch (...) {
        std::error_code ec;
        fs::remove(destination, ec);
        throw;
    }
}
Image luminance(const Image &im) {
    Image out;
    out.width = im.width;
    out.height = im.height;
    out.header = im.header;
    out.pixels.resize(im.plane());
    if (im.channels == 1)
        out.pixels = im.pixels;
    if (im.channels == 3)
        for (size_t i = 0; i < im.plane(); ++i)
            out.pixels[i] = float(0.2126 * im.pixels[i] + 0.7152 * im.pixels[i + im.plane()] +
                                  0.0722 * im.pixels[i + 2 * im.plane()]);
    return out;
}
} // namespace ss
