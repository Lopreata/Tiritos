// Monta o instalador: copia o programa do setup (setup_stub.exe) e cola no fim os arquivos do jogo, comprimidos.
// Uso: pack <setup_stub.exe> <saida.exe> <arquivo> <nome no jogo> [<arquivo> <nome no jogo> ...]
// O formato (lido por setup.cpp): [programa][arquivos][indice][rodape com "LPDBSETU"].
#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0A00          // Windows 10: a API de compressao (compressapi.h) so aparece a partir do 8
#define NTDDI_VERSION 0x0A000000
#include <windows.h>
#include <compressapi.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#pragma pack(push, 1)
struct Footer { unsigned long long payloadStart, indexOffset; unsigned count; char magic[8]; };
#pragma pack(pop)

static bool readAll(const char* path, std::vector<BYTE>& out) {
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    out.resize((size_t)n);
    bool ok = n == 0 || fread(out.data(), 1, out.size(), f) == out.size();
    fclose(f);
    return ok;
}

// LZMS comprime mais; se falhar, XPRESS; se tambem falhar, guarda sem comprimir (algo 0).
static unsigned compress(const std::vector<BYTE>& in, std::vector<BYTE>& out) {
    const DWORD algos[2] = { COMPRESS_ALGORITHM_LZMS, COMPRESS_ALGORITHM_XPRESS_HUFF };
    for (int a = 0; a < 2; ++a) {
        COMPRESSOR_HANDLE c = NULL;
        if (!CreateCompressor(algos[a], NULL, &c)) continue;
        SIZE_T need = 0;
        Compress(c, const_cast<BYTE*>(in.data()), in.size(), NULL, 0, &need);
        out.resize(need);
        SIZE_T got = 0;
        BOOL ok = need > 0 && Compress(c, const_cast<BYTE*>(in.data()), in.size(), out.data(), out.size(), &got);
        CloseCompressor(c);
        if (ok && got < in.size()) { out.resize(got); return algos[a]; }
    }
    out = in;
    return 0;
}

int main(int argc, char** argv) {
    if (argc < 5 || (argc - 3) % 2 != 0) {
        fprintf(stderr, "uso: pack <setup_stub.exe> <saida.exe> <arquivo> <nome> [<arquivo> <nome> ...]\n");
        return 1;
    }
    std::vector<BYTE> stub;
    if (!readAll(argv[1], stub)) { fprintf(stderr, "nao li %s\n", argv[1]); return 1; }
    FILE* out = fopen(argv[2], "wb");
    if (!out) { fprintf(stderr, "nao consegui criar %s\n", argv[2]); return 1; }
    fwrite(stub.data(), 1, stub.size(), out);

    struct Item { std::wstring name; unsigned long long offset, comp, orig; unsigned algo; };
    std::vector<Item> items;
    unsigned long long pos = stub.size();
    for (int i = 3; i + 1 < argc; i += 2) {
        std::vector<BYTE> data, packed;
        if (!readAll(argv[i], data)) { fprintf(stderr, "nao li %s\n", argv[i]); fclose(out); return 1; }
        Item it;
        it.algo = compress(data, packed);
        int wn = MultiByteToWideChar(CP_ACP, 0, argv[i + 1], -1, NULL, 0);
        std::vector<wchar_t> wname((size_t)wn);
        MultiByteToWideChar(CP_ACP, 0, argv[i + 1], -1, wname.data(), wn);
        it.name.assign(wname.data());
        for (size_t k = 0; k < it.name.size(); ++k) if (it.name[k] == L'/') it.name[k] = L'\\';
        it.offset = pos; it.comp = packed.size(); it.orig = data.size();
        fwrite(packed.data(), 1, packed.size(), out);
        pos += packed.size();
        printf("  %-24s %9llu -> %9llu bytes\n", argv[i + 1], it.orig, it.comp);
        items.push_back(it);
    }
    Footer ft;
    ft.payloadStart = stub.size();
    ft.indexOffset = pos;
    ft.count = (unsigned)items.size();
    memcpy(ft.magic, "LPDBSETU", 8);
    for (size_t i = 0; i < items.size(); ++i) {
        unsigned len = (unsigned)items[i].name.size();
        fwrite(&len, 4, 1, out);
        fwrite(items[i].name.c_str(), sizeof(wchar_t), len, out);
        fwrite(&items[i].offset, 8, 1, out);
        fwrite(&items[i].comp, 8, 1, out);
        fwrite(&items[i].orig, 8, 1, out);
        fwrite(&items[i].algo, 4, 1, out);
    }
    fwrite(&ft, sizeof(ft), 1, out);
    fclose(out);
    printf("instalador gerado: %s\n", argv[2]);
    return 0;
}
