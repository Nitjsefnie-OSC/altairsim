#include "host/framedump.h"

#include "core/crc32.h"

#include <cstdio>

namespace altair {

std::vector<uint8_t> frameRgb(const Surface& s, std::span<const Color> pal) {
    const auto           px = s.pixels();
    std::vector<uint8_t> out;
    if (s.format() == PixelFormat::Rgb32) {   // already colors: drop the alpha byte
        out.reserve(px.size() / 4 * 3);
        for (size_t i = 0; i < px.size(); i += 4) out.insert(out.end(), {px[i], px[i + 1], px[i + 2]});
        return out;
    }
    out.reserve(px.size() * 3);
    for (uint8_t i : px) {
        if (i < pal.size()) {
            out.push_back(pal[i].r);
            out.push_back(pal[i].g);
            out.push_back(pal[i].b);
        } else {
            out.push_back(0);
            out.push_back(0);
            out.push_back(0);
        }
    }
    return out;
}

std::vector<uint8_t> framePpm(const Surface& s, std::span<const Color> pal) {
    // The P6 header is ASCII: magic, whitespace, width, height, maxval, then exactly one
    // whitespace byte before the raster. Newlines throughout keep it readable in `head`.
    char hdr[64];
    int  n = std::snprintf(hdr, sizeof hdr, "P6\n%d %d\n255\n", s.width(), s.height());
    std::vector<uint8_t> out(hdr, hdr + n);
    std::vector<uint8_t> rgb = frameRgb(s, pal);
    out.insert(out.end(), rgb.begin(), rgb.end());
    return out;
}

bool writePpm(const std::string& path, const Surface& s, std::span<const Color> pal,
              std::string& err) {
    std::vector<uint8_t> bytes = framePpm(s, pal);
    std::FILE*           f     = std::fopen(path.c_str(), "wb");
    if (!f) {
        err = "cannot write " + path;
        return false;
    }
    bool ok = std::fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
    if (std::fclose(f) != 0) ok = false;
    if (!ok) err = "short write to " + path;
    return ok;
}

std::string frameText(const Surface& s, const TextGridOpts& o) {
    const int  xs = o.xStep < 1 ? 1 : o.xStep;
    const int  ys = o.yStep < 1 ? 1 : o.yStep;
    const auto px = s.pixels();
    std::string out;
    out.reserve((size_t)((s.width() + xs - 1) / xs + 1) * (size_t)((s.height() + ys - 1) / ys));
    for (int y = 0; y < s.height(); y += ys) {
        for (int x = 0; x < s.width(); x += xs) {
            const size_t at = (size_t)y * (size_t)s.pitch() + (size_t)x * (size_t)bytesPerPixel(s.format());
            if (s.format() == PixelFormat::Rgb32) {
                const bool black = px[at] == 0 && px[at + 1] == 0 && px[at + 2] == 0;
                out.push_back(black ? (o.legend.empty() ? '?' : o.legend[0]) : '#');
                continue;
            }
            uint8_t i = px[at];
            out.push_back(i < o.legend.size() ? o.legend[i] : '?');
        }
        out.push_back('\n');
    }
    return out;
}

uint32_t frameCrc(const Surface& s, std::span<const Color> pal) {
    std::vector<uint8_t> rgb = frameRgb(s, pal);
    return crc32(rgb);
}

} // namespace altair
