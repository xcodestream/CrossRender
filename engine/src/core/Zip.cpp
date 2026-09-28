#include "core/Zip.h"

#include "crossrender/core/Log.h"

#include <cstring>

namespace crossrender {
namespace {

// ---------------------------------------------------------------------------
// Побитовый читатель (LSB-first, как требует DEFLATE).
// Инвариант: после любого чтения в bitcnt остаётся не более 7 бит, что
// позволяет выравниваться на границу байта простым сбросом буфера.
// ---------------------------------------------------------------------------
struct BitReader {
    const u8* data;
    usize size;
    usize pos = 0;
    u32 bitbuf = 0;
    int bitcnt = 0;

    // Читает need бит (0..16); возвращает -1, если данные кончились.
    int Bits(int need) {
        while (bitcnt < need) {
            if (pos >= size) return -1;
            bitbuf |= static_cast<u32>(data[pos++]) << bitcnt;
            bitcnt += 8;
        }
        int val = static_cast<int>(bitbuf & ((1u << need) - 1));
        bitbuf >>= need;
        bitcnt -= need;
        return val;
    }
};

// Канонический код Хаффмана: количество кодов каждой длины + символы,
// упорядоченные по коду. Схема декодирования - по алгоритму из puff (Марк
// Адлер): по одному биту за шаг, без явного дерева.
struct Huffman {
    int counts[16];
    int symbols[288];
};

// Строит код из таблицы длин; возвращает число "недостроенных" кодов
// (положительное значение допустимо только для кодов расстояний),
// -1 при переполнении кода.
int HuffmanBuild(Huffman* h, const u8* lengths, int n) {
    for (int i = 0; i < 16; ++i) h->counts[i] = 0;
    for (int i = 0; i < n; ++i) h->counts[lengths[i]]++;
    if (h->counts[0] == n) return 0;  // пустой код - все длины нулевые
    int left = 1;
    for (int len = 1; len <= 15; ++len) {
        left <<= 1;
        left -= h->counts[len];
        if (left < 0) return -1;
    }
    int offs[16];
    offs[1] = 0;
    for (int len = 1; len < 15; ++len) offs[len + 1] = offs[len] + h->counts[len];
    for (int i = 0; i < n; ++i) {
        if (lengths[i]) h->symbols[offs[lengths[i]]++] = static_cast<i16>(i);
    }
    return left;
}

int HuffmanDecode(BitReader& br, const Huffman& h) {
    int code = 0, first = 0, index = 0;
    for (int len = 1; len <= 15; ++len) {
        int b = br.Bits(1);
        if (b < 0) return -1;
        code |= b;
        int count = h.counts[len];
        if (code - count < first) return h.symbols[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    return -1;
}

// Таблицы длин/расстояний из RFC 1951, раздел 3.2.5.
const u16 kLenBase[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
                          35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
const u8 kLenExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
                          3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
const u16 kDistBase[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
                           257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145,
                           8193, 12289, 16385, 24577};
const u8 kDistExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
                           7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
// Порядок длин кодов кода длин (RFC 1951, раздел 3.2.7).
const u8 kCodeOrder[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};

bool InflateBlocks(BitReader& br, ByteBuffer* out, usize expectedSize) {
    for (;;) {
        int last = br.Bits(1);
        int type = br.Bits(2);
        if (last < 0 || type < 0) return false;

        if (type == 0) {
            // Сохранённый блок: сброс до границы байта, LEN/NLEN, сырые байты.
            br.bitbuf = 0;
            br.bitcnt = 0;
            int len = br.Bits(8);
            if (len < 0) return false;
            int hi = br.Bits(8);
            if (hi < 0) return false;
            len |= hi << 8;
            int nlen = br.Bits(8);
            if (nlen < 0) return false;
            hi = br.Bits(8);
            if (hi < 0) return false;
            nlen |= hi << 8;
            if ((len ^ 0xFFFF) != nlen) return false;
            for (int i = 0; i < len; ++i) {
                int b = br.Bits(8);
                if (b < 0) return false;
                out->push_back(static_cast<u8>(b));
            }
        } else {
            Huffman litlen, dist;
            if (type == 1) {
                // Фиксированный код Хаффмана (RFC 1951, раздел 3.2.6).
                u8 lens[288];
                int i = 0;
                for (; i < 144; ++i) lens[i] = 8;
                for (; i < 256; ++i) lens[i] = 9;
                for (; i < 280; ++i) lens[i] = 7;
                for (; i < 288; ++i) lens[i] = 8;
                HuffmanBuild(&litlen, lens, 288);
                u8 dlens[30];
                for (i = 0; i < 30; ++i) dlens[i] = 5;
                HuffmanBuild(&dist, dlens, 30);
            } else if (type == 2) {
                // Динамический код: длины кода длин, затем длины литералов/расстояний.
                int nlen = br.Bits(5) + 257;
                int ndist = br.Bits(5) + 1;
                int ncode = br.Bits(4) + 4;
                if (nlen < 0 || ndist < 0 || ncode < 0) return false;
                if (nlen > 286 || ndist > 30) return false;
                u8 clens[19] = {0};
                for (int i = 0; i < ncode; ++i) {
                    int b = br.Bits(3);
                    if (b < 0) return false;
                    clens[kCodeOrder[i]] = static_cast<u8>(b);
                }
                Huffman cl;
                if (HuffmanBuild(&cl, clens, 19) < 0) return false;
                u8 lens[320] = {0};
                int index = 0;
                while (index < nlen + ndist) {
                    int sym = HuffmanDecode(br, cl);
                    if (sym < 0) return false;
                    if (sym < 16) {
                        lens[index++] = static_cast<u8>(sym);
                        continue;
                    }
                    int len = 0, rep;
                    if (sym == 16) {
                        if (index == 0) return false;
                        len = lens[index - 1];
                        rep = 3 + br.Bits(2);
                    } else if (sym == 17) {
                        rep = 3 + br.Bits(3);
                    } else {
                        rep = 11 + br.Bits(7);
                    }
                    if (rep < 0 || index + rep > nlen + ndist) return false;
                    while (rep--) lens[index++] = static_cast<u8>(len);
                }
                if (lens[256] == 0) return false;  // нет кода конца блока
                if (HuffmanBuild(&litlen, lens, nlen) < 0) return false;
                // Неполный код расстояний допустим: расстояния могут не встретиться.
                if (HuffmanBuild(&dist, lens + nlen, ndist) < 0) return false;
            } else {
                return false;  // тип 3 зарезервирован
            }

            // Декодирование LZ77-последовательности блока.
            for (;;) {
                int sym = HuffmanDecode(br, litlen);
                if (sym < 0) return false;
                if (sym < 256) {
                    out->push_back(static_cast<u8>(sym));
                } else if (sym == 256) {
                    break;
                } else {
                    sym -= 257;
                    if (sym >= 29) return false;
                    int lenExtra = br.Bits(kLenExtra[sym]);
                    if (lenExtra < 0) return false;
                    usize len = kLenBase[sym] + static_cast<usize>(lenExtra);
                    int dsym = HuffmanDecode(br, dist);
                    if (dsym < 0 || dsym >= 30) return false;
                    int distExtra = br.Bits(kDistExtra[dsym]);
                    if (distExtra < 0) return false;
                    usize distance = kDistBase[dsym] + static_cast<usize>(distExtra);
                    if (distance > out->size()) return false;  // ссылка до начала вывода
                    usize from = out->size() - distance;
                    for (usize i = 0; i < len; ++i) out->push_back((*out)[from + i]);
                }
                if (expectedSize && out->size() > expectedSize) return false;
            }
        }

        if (expectedSize && out->size() > expectedSize) return false;
        if (last) return true;
    }
}

u16 Read16(const u8* p) { return static_cast<u16>(p[0] | (p[1] << 8)); }
u32 Read32(const u8* p) {
    return static_cast<u32>(p[0]) | (static_cast<u32>(p[1]) << 8) |
           (static_cast<u32>(p[2]) << 16) | (static_cast<u32>(p[3]) << 24);
}

struct ZipEntry {
    std::string name;
    u16 method = 0;
    u32 compressedSize = 0;
    u32 uncompressedSize = 0;
    u32 localOffset = 0;
    bool encrypted = false;
};

// Находит конец записи центрального каталога (EOCD) с учётом комментария архива.
const u8* FindEocd(const ByteBuffer& zip, u16* count, u32* cdSize, u32* cdOfs) {
    const usize n = zip.size();
    if (n < 22) return nullptr;
    const usize maxBack = n < 22 + 65535 ? n : 22 + 65535;
    const u8* base = zip.data();
    const usize first = n - maxBack;
    for (usize i = n - 22;; --i) {
        if (Read32(base + i) == 0x06054b50 &&
            i + 22 + Read16(base + i + 20) == n) {
            *count = Read16(base + i + 10);
            *cdSize = Read32(base + i + 12);
            *cdOfs = Read32(base + i + 16);
            return base + i;
        }
        if (i == first) break;
    }
    return nullptr;
}

bool ParseCentralDirectory(const ByteBuffer& zip, std::vector<ZipEntry>* out) {
    u16 count = 0;
    u32 cdSize = 0, cdOfs = 0;
    if (!FindEocd(zip, &count, &cdSize, &cdOfs)) return false;
    if (count == 0xFFFF || cdOfs == 0xFFFFFFFFu || cdSize == 0xFFFFFFFFu) {
        ENG_LOGE("fs", "assets.dat: ZIP64 не поддерживается");
        return false;
    }
    const usize n = zip.size();
    if (static_cast<usize>(cdOfs) > n || cdSize > n - static_cast<usize>(cdOfs)) return false;
    const u8* p = zip.data() + cdOfs;
    for (u16 i = 0; i < count; ++i) {
        if (p + 46 > zip.data() + n || Read32(p) != 0x02014b50) return false;
        ZipEntry entry;
        entry.encrypted = (Read16(p + 8) & 1) != 0;
        entry.method = Read16(p + 10);
        entry.compressedSize = Read32(p + 20);
        entry.uncompressedSize = Read32(p + 24);
        u16 nameLen = Read16(p + 28);
        u16 extraLen = Read16(p + 30);
        u16 commentLen = Read16(p + 32);
        entry.localOffset = Read32(p + 42);
        const usize offset = static_cast<usize>(p - zip.data());
        const usize directoryEnd = static_cast<usize>(cdOfs) + cdSize;
        if (offset > directoryEnd || 46 > directoryEnd - offset) return false;
        const usize recordSize = 46u + static_cast<usize>(nameLen) +
                                 static_cast<usize>(extraLen) + static_cast<usize>(commentLen);
        if (recordSize > directoryEnd - offset) return false;
        entry.name.assign(reinterpret_cast<const char*>(p + 46), nameLen);
        p += recordSize;
        out->push_back(std::move(entry));
    }
    return true;
}

}  // namespace

bool Inflate(const u8* data, usize size, usize expectedSize, ByteBuffer* out) {
    if (!data || size == 0 || !out) return false;
    if (expectedSize) out->reserve(out->size() + expectedSize);
    BitReader br{data, size};
    return InflateBlocks(br, out, expectedSize);
}

bool ZipListEntries(const ByteBuffer& zip, std::vector<std::string>* out) {
    if (!out) return false;
    std::vector<ZipEntry> entries;
    if (!ParseCentralDirectory(zip, &entries)) return false;
    out->clear();
    out->reserve(entries.size());
    for (const ZipEntry& e : entries) {
        if (!e.name.empty() && e.name.back() == '/') continue;  // запись-каталог
        out->push_back(e.name);
    }
    return true;
}

bool ZipReadFile(const ByteBuffer& zip, const std::string& name, ByteBuffer* out) {
    if (!out || name.empty()) return false;
    std::vector<ZipEntry> entries;
    if (!ParseCentralDirectory(zip, &entries)) return false;
    const ZipEntry* found = nullptr;
    for (const ZipEntry& e : entries) {
        if (e.name == name) {
            found = &e;
            break;
        }
    }
    if (!found || found->encrypted) return false;
    if (found->uncompressedSize == 0) {
        out->clear();
        return true;  // пустая запись
    }
    const usize n = zip.size();
    usize off = found->localOffset;
    if (off > n || 30 > n - off || Read32(zip.data() + off) != 0x04034b50) return false;
    u16 nameLen = Read16(zip.data() + off + 26);
    u16 extraLen = Read16(zip.data() + off + 28);
    if (nameLen > n - off - 30u || extraLen > n - off - 30u - nameLen) return false;
    const usize dataAt = off + 30u + nameLen + extraLen;
    if (found->compressedSize > n - dataAt) return false;
    const u8* data = zip.data() + dataAt;
    if (found->method == 0) {
        if (found->compressedSize != found->uncompressedSize) return false;
        out->assign(data, data + found->compressedSize);
        return true;
    }
    if (found->method != 8) {
        ENG_LOGE("fs", "assets.dat: метод сжатия %u записи '%s' не поддерживается",
                 found->method, name.c_str());
        return false;
    }
    out->clear();
    if (!Inflate(data, found->compressedSize, found->uncompressedSize, out)) {
        ENG_LOGE("fs", "assets.dat: повреждённая deflate-запись '%s'", name.c_str());
        return false;
    }
    return out->size() == found->uncompressedSize;
}

}  // namespace crossrender
