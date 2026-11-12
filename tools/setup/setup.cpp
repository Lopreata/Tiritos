// Instalador do Tiritos (Tiritos_Setup.exe) e desinstalador (uninstall.exe).
//
// O instalador e este programa com os arquivos do jogo colados no fim (comprimidos por tools/setup/pack.exe).
// Ele instala em %LOCALAPPDATA%\Programs\Tiritos (sem precisar de administrador), cria atalhos no Menu Iniciar e na
// Area de Trabalho, registra o jogo em "Aplicativos instalados" e, se a pessoa quiser, libera o jogo no firewall
// (para a partida em rede). O uninstall.exe e o mesmo programa sem os arquivos: le a lista do que foi instalado
// (uninstall.dat) e apaga so isso.
//
// Opcoes (para testes ou instalacao automatica):
//   /S                 sem perguntas            /D=<pasta>        instala em outra pasta
//   /noshortcuts       sem atalhos              /shortcutdir=<p>  atalhos nesta pasta (em vez do Menu e da Area de Trabalho)
//   /regkey=<nome>     nome da chave de registro (padrao Tiritos)    /noreg   nao registra em "Aplicativos instalados"
//   /uninstall         desinstala (o uninstall.exe faz isso mesmo sem a opcao)

#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0A00          // Windows 10: a API de compressao (compressapi.h) so aparece a partir do 8
#define NTDDI_VERSION 0x0A000000
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <objbase.h>
#include <compressapi.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static const wchar_t* TITLE = L"Tiritos - Instalação";
static const wchar_t* GAME_EXE = L"Tiritos.exe";
static const char MAGIC[8] = { 'L', 'P', 'D', 'B', 'S', 'E', 'T', 'U' };

#pragma pack(push, 1)
struct Footer { unsigned long long payloadStart, indexOffset; unsigned count; char magic[8]; };
#pragma pack(pop)

struct Entry { std::wstring name; unsigned long long offset, compSize, origSize; unsigned algo; };

struct Options {
    bool silent = false, uninstall = false, shortcuts = true, reg = true;
    std::wstring dir, shortcutDir, regKey = L"Tiritos";
};

static std::wstring selfPath() {
    wchar_t buf[MAX_PATH * 4];
    DWORD n = GetModuleFileNameW(NULL, buf, (DWORD)(sizeof(buf) / sizeof(buf[0])));
    return std::wstring(buf, n);
}

static std::wstring dirOf(const std::wstring& p) {
    size_t s = p.find_last_of(L"\\/");
    return s == std::wstring::npos ? L"." : p.substr(0, s);
}

static std::wstring knownFolder(REFKNOWNFOLDERID id) {
    PWSTR p = NULL;
    std::wstring out;
    if (SUCCEEDED(SHGetKnownFolderPath(id, 0, NULL, &p))) out = p;
    CoTaskMemFree(p);
    return out;
}

static int ask(const Options& o, const std::wstring& text, UINT flags) {
    if (o.silent) return (flags & MB_YESNO) ? IDYES : IDOK;
    return MessageBoxW(NULL, text.c_str(), TITLE, flags);
}

static void fail(const Options& o, const std::wstring& text) {
    if (!o.silent) MessageBoxW(NULL, text.c_str(), TITLE, MB_OK | MB_ICONERROR);
}

// Cria a pasta e todas as pastas acima dela que faltarem.
static bool makeDirs(const std::wstring& dir) {
    if (dir.empty()) return false;
    DWORD a = GetFileAttributesW(dir.c_str());
    if (a != INVALID_FILE_ATTRIBUTES) return (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
    std::wstring parent = dirOf(dir);
    if (parent != dir && parent.size() > 2) makeDirs(parent);
    return CreateDirectoryW(dir.c_str(), NULL) || GetLastError() == ERROR_ALREADY_EXISTS;
}

// ---- leitura dos arquivos colados no fim do instalador
static bool readPayload(const std::wstring& exe, std::vector<Entry>& entries, unsigned long long& payloadStart) {
    FILE* f = _wfopen(exe.c_str(), L"rb");
    if (!f) return false;
    Footer ft;
    bool ok = _fseeki64(f, -(long long)sizeof(Footer), SEEK_END) == 0 && fread(&ft, sizeof(ft), 1, f) == 1 &&
              memcmp(ft.magic, MAGIC, 8) == 0 && _fseeki64(f, (long long)ft.indexOffset, SEEK_SET) == 0;
    for (unsigned i = 0; ok && i < ft.count; ++i) {
        unsigned len = 0;
        Entry e;
        ok = fread(&len, 4, 1, f) == 1 && len < 1024;
        if (!ok) break;
        std::vector<wchar_t> name(len);
        ok = (len == 0 || fread(name.data(), sizeof(wchar_t), len, f) == len) &&
             fread(&e.offset, 8, 1, f) == 1 && fread(&e.compSize, 8, 1, f) == 1 &&
             fread(&e.origSize, 8, 1, f) == 1 && fread(&e.algo, 4, 1, f) == 1;
        e.name.assign(name.begin(), name.end());
        entries.push_back(e);
    }
    fclose(f);
    payloadStart = ok ? ft.payloadStart : 0;
    return ok && !entries.empty();
}

static bool extractFile(FILE* src, const Entry& e, const std::wstring& outPath) {
    std::vector<BYTE> comp((size_t)e.compSize);
    if (_fseeki64(src, (long long)e.offset, SEEK_SET) != 0) return false;
    if (e.compSize && fread(comp.data(), 1, comp.size(), src) != comp.size()) return false;
    std::vector<BYTE> data;
    if (e.algo == 0) {
        data.swap(comp);
    } else {
        DECOMPRESSOR_HANDLE d = NULL;
        if (!CreateDecompressor(e.algo, NULL, &d)) return false;
        data.resize((size_t)e.origSize);
        SIZE_T got = 0;
        BOOL ok = Decompress(d, comp.data(), comp.size(), data.data(), data.size(), &got);
        CloseDecompressor(d);
        if (!ok || got != e.origSize) return false;
    }
    makeDirs(dirOf(outPath));
    FILE* out = _wfopen(outPath.c_str(), L"wb");
    if (!out) return false;
    bool ok = data.empty() || fwrite(data.data(), 1, data.size(), out) == data.size();
    ok = fclose(out) == 0 && ok;
    return ok;
}

// Copia o comeco do proprio instalador (o programa, sem os arquivos do jogo): vira o uninstall.exe.
static bool writeUninstaller(const std::wstring& self, unsigned long long stubSize, const std::wstring& outPath) {
    FILE* in = _wfopen(self.c_str(), L"rb");
    if (!in) return false;
    std::vector<char> buf((size_t)stubSize);
    bool ok = fread(buf.data(), 1, buf.size(), in) == buf.size();
    fclose(in);
    FILE* out = _wfopen(outPath.c_str(), L"wb");
    if (!out) return false;
    ok = ok && fwrite(buf.data(), 1, buf.size(), out) == buf.size();
    return fclose(out) == 0 && ok;
}

static bool createShortcut(const std::wstring& link, const std::wstring& target, const std::wstring& workDir) {
    IShellLinkW* sl = NULL;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER, IID_IShellLinkW, (void**)&sl))) return false;
    sl->SetPath(target.c_str());
    sl->SetWorkingDirectory(workDir.c_str());
    sl->SetIconLocation(target.c_str(), 0);
    sl->SetDescription(L"Tiritos");
    IPersistFile* pf = NULL;
    bool ok = SUCCEEDED(sl->QueryInterface(IID_IPersistFile, (void**)&pf)) && SUCCEEDED(pf->Save(link.c_str(), TRUE));
    if (pf) pf->Release();
    sl->Release();
    return ok;
}

static void regString(HKEY k, const wchar_t* name, const std::wstring& v) {
    RegSetValueExW(k, name, 0, REG_SZ, (const BYTE*)v.c_str(), (DWORD)((v.size() + 1) * sizeof(wchar_t)));
}

static void regDword(HKEY k, const wchar_t* name, DWORD v) {
    RegSetValueExW(k, name, 0, REG_DWORD, (const BYTE*)&v, sizeof(v));
}

static std::wstring uninstallKey(const std::wstring& name) {
    return L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\" + name;
}

// Roda um comando como administrador (o Windows mostra o pedido de permissao). Devolve true se terminou com sucesso.
static bool runElevated(const std::wstring& params) {
    SHELLEXECUTEINFOW sei;
    memset(&sei, 0, sizeof(sei));
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.lpVerb = L"runas";
    sei.lpFile = L"cmd.exe";
    sei.lpParameters = params.c_str();
    sei.nShow = SW_HIDE;
    if (!ShellExecuteExW(&sei) || !sei.hProcess) return false;
    WaitForSingleObject(sei.hProcess, 60000);
    DWORD code = 1;
    GetExitCodeProcess(sei.hProcess, &code);
    CloseHandle(sei.hProcess);
    return code == 0;
}

// ---- lista do que foi instalado (uninstall.dat): uma linha por item, em UTF-16
//   F <arquivo relativo a pasta>   L <atalho>   R <chave de registro>   W (regra de firewall)
static void writeList(const std::wstring& path, const std::vector<std::wstring>& lines) {
    FILE* f = _wfopen(path.c_str(), L"wb");
    if (!f) return;
    for (size_t i = 0; i < lines.size(); ++i) {
        fwrite(lines[i].c_str(), sizeof(wchar_t), lines[i].size(), f);
        wchar_t nl = L'\n';
        fwrite(&nl, sizeof(wchar_t), 1, f);
    }
    fclose(f);
}

static std::vector<std::wstring> readList(const std::wstring& path) {
    std::vector<std::wstring> lines;
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) return lines;
    std::wstring cur;
    wchar_t c;
    while (fread(&c, sizeof(wchar_t), 1, f) == 1) {
        if (c == L'\n') { if (!cur.empty()) lines.push_back(cur); cur.clear(); }
        else cur += c;
    }
    if (!cur.empty()) lines.push_back(cur);
    fclose(f);
    return lines;
}

// ---- instalar
static int install(const Options& o) {
    std::wstring self = selfPath();
    std::vector<Entry> entries;
    unsigned long long stubSize = 0;
    if (!readPayload(self, entries, stubSize)) {
        fail(o, L"Os arquivos do jogo não foram encontrados dentro do instalador. Baixe o Tiritos_Setup.exe de novo.");
        return 2;
    }
    std::wstring dir = o.dir;
    if (dir.empty()) dir = knownFolder(FOLDERID_LocalAppData) + L"\\Programs\\Tiritos";
    unsigned long long total = 0;
    for (size_t i = 0; i < entries.size(); ++i) total += entries[i].origSize;

    if (ask(o, L"Instalar o Tiritos?\n\nPasta: " + dir + L"\nEspaço necessário: " + std::to_wstring(total / (1024 * 1024) + 1) +
               L" MB\n\nSerão criados atalhos no Menu Iniciar e na Área de Trabalho.",
            MB_YESNO | MB_ICONQUESTION) != IDYES)
        return 1;

    if (!makeDirs(dir)) { fail(o, L"Não foi possível criar a pasta:\n" + dir); return 3; }
    FILE* src = _wfopen(self.c_str(), L"rb");
    if (!src) { fail(o, L"Não foi possível ler o instalador."); return 3; }
    std::vector<std::wstring> list;
    for (size_t i = 0; i < entries.size(); ++i) {
        std::wstring out = dir + L"\\" + entries[i].name;
        if (!extractFile(src, entries[i], out)) {
            fclose(src);
            fail(o, L"Não foi possível gravar:\n" + out + L"\n\nSe o jogo estiver aberto, feche-o e rode o instalador de novo.");
            return 4;
        }
        list.push_back(L"F " + entries[i].name);
    }
    fclose(src);
    std::wstring uninst = dir + L"\\uninstall.exe";
    if (!writeUninstaller(self, stubSize, uninst)) { fail(o, L"Não foi possível criar o desinstalador."); return 4; }
    std::wstring game = dir + L"\\" + GAME_EXE;

    if (o.shortcuts) {
        std::vector<std::wstring> places;
        if (!o.shortcutDir.empty()) places.push_back(o.shortcutDir);
        else { places.push_back(knownFolder(FOLDERID_Programs)); places.push_back(knownFolder(FOLDERID_Desktop)); }
        for (size_t i = 0; i < places.size(); ++i) {
            if (places[i].empty()) continue;
            makeDirs(places[i]);
            std::wstring link = places[i] + L"\\Tiritos.lnk";
            if (createShortcut(link, game, dir)) list.push_back(L"L " + link);
        }
    }

    if (o.reg) {
        HKEY k;
        if (RegCreateKeyExW(HKEY_CURRENT_USER, uninstallKey(o.regKey).c_str(), 0, NULL, 0, KEY_WRITE, NULL, &k, NULL) == ERROR_SUCCESS) {
            regString(k, L"DisplayName", L"Tiritos");
            regString(k, L"DisplayVersion", L"1.4.0");
            regString(k, L"DisplayIcon", game);
            regString(k, L"InstallLocation", dir);
            regString(k, L"UninstallString", L"\"" + uninst + L"\" /uninstall");
            regString(k, L"QuietUninstallString", L"\"" + uninst + L"\" /uninstall /S");
            regDword(k, L"EstimatedSize", (DWORD)(total / 1024));
            regDword(k, L"NoModify", 1);
            regDword(k, L"NoRepair", 1);
            RegCloseKey(k);
            list.push_back(L"R " + o.regKey);
        }
    }

    // a partida em rede precisa que o firewall deixe o jogo receber conexoes (o Windows pede permissao de administrador)
    if (!o.silent && MessageBoxW(NULL,
            L"Liberar o Tiritos no Firewall do Windows?\n\nIsso é necessário para jogar em rede (LAN) com outro "
            L"computador. O Windows vai pedir permissão de administrador.\n\nSe preferir, escolha Não: o próprio Windows "
            L"pergunta na primeira vez que você criar uma partida.",
            TITLE, MB_YESNO | MB_ICONQUESTION) == IDYES) {
        std::wstring cmd = L"/c netsh advfirewall firewall delete rule name=\"Tiritos\" >nul 2>&1 & "
                           L"netsh advfirewall firewall add rule name=\"Tiritos\" dir=in action=allow program=\"" + game +
                           L"\" enable=yes profile=private,domain";
        if (runElevated(cmd)) list.push_back(L"W");
    }

    writeList(dir + L"\\uninstall.dat", list);

    if (!o.silent && MessageBoxW(NULL, L"O Tiritos foi instalado!\n\nAbrir o jogo agora?", TITLE, MB_YESNO | MB_ICONINFORMATION) == IDYES)
        ShellExecuteW(NULL, L"open", game.c_str(), NULL, dir.c_str(), SW_SHOWNORMAL);
    return 0;
}

// ---- desinstalar: apaga so o que esta na lista, e depois o proprio uninstall.exe e a pasta (se ficar vazia)
static int uninstall(const Options& o) {
    std::wstring self = selfPath();
    std::wstring dir = dirOf(self);
    std::vector<std::wstring> list = readList(dir + L"\\uninstall.dat");
    if (list.empty()) { fail(o, L"Não encontrei uma instalação do Tiritos nesta pasta."); return 2; }
    if (ask(o, L"Desinstalar o Tiritos?\n\nPasta: " + dir, MB_YESNO | MB_ICONQUESTION) != IDYES) return 1;

    // apaga o Tiritos.exe antes de tudo: se o jogo estiver aberto o Windows nao deixa, e ai nada mais e apagado
    // (senao ficaria meio desinstalado)
    std::wstring game = dir + L"\\" + GAME_EXE;
    if (!DeleteFileW(game.c_str()) && GetLastError() != ERROR_FILE_NOT_FOUND) {
        fail(o, L"O Tiritos está aberto. Feche o jogo e tente desinstalar de novo.");
        return 3;
    }

    bool busy = false;
    std::vector<std::wstring> subdirs;
    for (size_t i = 0; i < list.size(); ++i) {
        const std::wstring& l = list[i];
        std::wstring arg = l.size() > 2 ? l.substr(2) : L"";
        if (l[0] == L'F') {
            if (arg.find(L"..") != std::wstring::npos) continue;          // so dentro da pasta do jogo
            std::wstring p = dir + L"\\" + arg;
            if (!DeleteFileW(p.c_str()) && GetLastError() != ERROR_FILE_NOT_FOUND) busy = true;
            if (arg.find(L'\\') != std::wstring::npos) subdirs.push_back(dirOf(p));
        } else if (l[0] == L'L') {
            DeleteFileW(arg.c_str());
        } else if (l[0] == L'R') {
            RegDeleteKeyW(HKEY_CURRENT_USER, uninstallKey(arg).c_str());
        } else if (l[0] == L'W' && !o.silent) {
            runElevated(L"/c netsh advfirewall firewall delete rule name=\"Tiritos\"");
        }
    }
    if (busy) { fail(o, L"Alguns arquivos não puderam ser apagados. Feche o Tiritos e tente de novo."); return 3; }
    for (size_t i = 0; i < subdirs.size(); ++i) RemoveDirectoryW(subdirs[i].c_str());   // so se ficaram vazias
    DeleteFileW((dir + L"\\uninstall.dat").c_str());

    // um programa nao consegue apagar a si mesmo: pede ao cmd para apagar depois que este processo terminar
    // (o cmd roda na pasta de cima: uma pasta em uso como pasta atual nao pode ser removida)
    std::wstring cmd = L"cmd.exe /c ping 127.0.0.1 -n 3 >nul & del /f /q \"" + self + L"\" & rmdir \"" + dir + L"\"";
    STARTUPINFOW si;
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi;
    std::vector<wchar_t> buf(cmd.begin(), cmd.end());
    buf.push_back(0);
    if (CreateProcessW(NULL, buf.data(), NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, dirOf(dir).c_str(), &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
    if (!o.silent) MessageBoxW(NULL, L"O Tiritos foi desinstalado.", TITLE, MB_OK | MB_ICONINFORMATION);
    return 0;
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    Options o;
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; argv && i < argc; ++i) {
        std::wstring a = argv[i];
        if (a == L"/S") o.silent = true;
        else if (a == L"/uninstall") o.uninstall = true;
        else if (a == L"/noshortcuts") o.shortcuts = false;
        else if (a == L"/noreg") o.reg = false;
        else if (a.compare(0, 3, L"/D=") == 0) o.dir = a.substr(3);
        else if (a.compare(0, 13, L"/shortcutdir=") == 0) o.shortcutDir = a.substr(13);
        else if (a.compare(0, 8, L"/regkey=") == 0) o.regKey = a.substr(8);
    }
    if (argv) LocalFree(argv);
    while (!o.dir.empty() && (o.dir.back() == L'\\' || o.dir.back() == L'/')) o.dir.pop_back();

    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    // sem arquivos colados no fim, este programa e o uninstall.exe
    std::vector<Entry> probe;
    unsigned long long stub = 0;
    bool hasPayload = readPayload(selfPath(), probe, stub);
    int r = (o.uninstall || !hasPayload) ? uninstall(o) : install(o);
    CoUninitialize();
    return r;
}
