// Tiritos -- combate em um galpao fechado: o jogador contra um bot do genero oposto, ou contra outra pessoa na rede.
// Win32 + OpenGL 1.x (fixed pipeline): nao precisa de nenhuma biblioteca externa.
//
// Os personagens vem de assets/male.mdl e assets/female.mdl (gerados por tools/convert_model.py).
//
// Controles:
//   Selecao:  A D / setas  trocar personagem        Enter  continuar        Esc  menu
//             W S  estilo de cabelo   Q E  cor do cabelo   B  barba (homem)   T  roupa
//   Nome:     digitar o nome (aparece sobre a cabeca no jogo)    Enter  jogar    Esc  voltar
//   Jogo:     W A S D  andar             Mouse / setas  girar a camera
//             Shift    correr            Espaco  pular             Esc  menu
//             1 / 2    mao nua / arma    Botao esquerdo  atirar    R  recarregar
//
// Modos: contra o bot, ou contra outra pessoa na rede local (UDP na porta 27015; um cria a partida, o outro entra).
//
// Testes sem jogar: Tiritos.exe --anim-test <prefixo> (ciclo de passos) e --sim-test <arquivo> (combate).

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define _WINSOCK_DEPRECATED_NO_WARNINGS
#include <winsock2.h>
#include <windows.h>
#include <GL/gl.h>
#include <algorithm>
#include <cstddef>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#ifdef _MSC_VER
#pragma comment(lib, "opengl32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "ws2_32.lib")
#endif

#ifndef GL_GENERATE_MIPMAP
#define GL_GENERATE_MIPMAP 0x8191
#endif

// Em notebooks com duas placas de video, pede a dedicada (NVIDIA / AMD) em vez da integrada, que e bem mais lenta.
extern "C" {
    __declspec(dllexport) DWORD NvOptimusEnablement = 0x00000001;
    __declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
}

static const float PI = 3.14159265f;

// ---------------------------------------------------------------- estado global

static bool g_keys[256];
static bool g_prevKeys[256];
static bool g_focused = true;
static int  g_width = 1280, g_height = 720;

static bool pressed(int k) { return g_keys[k] && !g_prevKeys[k]; }

enum ObKind { OB_WALL, OB_PILLAR, OB_COVER, OB_CONTAINER, OB_CRATE, OB_BARRIER, OB_ROCK };   // define a textura e o acabamento
struct Obstacle { float x, z, hw, hd, h, r, g, b; int kind; };
static const int MAX_OBS = 160;
static Obstacle g_obs[MAX_OBS];
static int g_numObs = 0;

// Cenario: uma arena de 150 x 110; as 4 primeiras caixas sao as paredes. Ha tres mapas, do mesmo tamanho e com os
// mesmos pontos de nascimento: o galpao (fechado, com teto), o deserto (a ceu aberto) e a sala branca.
static const float ROOM_X = 75.0f, ROOM_Z = 55.0f;   // meia largura e meia profundidade do piso
static const float ROOM_H = 16.0f;                    // altura do teto (galpao e sala branca)
static const int   NUM_WALLS = 4;
enum MapId { MAP_GALPAO, MAP_DESERTO, MAP_BRANCO, NUM_MAPS };
static const char* const MAP_NAMES[NUM_MAPS] = { "Galp\xE3o", "Deserto", "Sala branca" };
static const char* const MAP_INFO[NUM_MAPS] = {
    "Fechado e iluminado por cima, com muros, caixotes e containers.",
    "A c\xE9u aberto e \xE1rido, entre rochas, ru\xEDnas e sacos de areia.",
    "Tudo branco e com n\xE9voa: de longe quase n\xE3o se v\xEA nada." };
static int   g_map = MAP_GALPAO;                      // mapa escolhido (na rede, o do anfitriao)
static int   g_worldMap = -1;                         // mapa que esta montado agora (ensureWorld)
static float g_ceil = ROOM_H;                         // altura em que tiro e camera batem no teto (o deserto nao tem)
static const float DESERT_WALL_H = 7.0f;              // muralha em volta do deserto

struct Player {
    float x = 0, y = 0, z = 0;
    float vy = 0;
    float face = 0;        // direcao do corpo (radianos, 0 = +Z)
    float vx = 0, vz = 0;  // velocidade horizontal (suavizada)
    float walkPhase = 0;   // fase do passo, em ciclos (0..1)
    float swing = 0;       // 0 = parado, 1 = andando (suavizado)
    float speed = 0;       // velocidade horizontal em unidades do modelo (m/s de um humano de 1,8 m)
    bool  onGround = true;
    float air = 0;         // 0 = no chao, 1 = no ar (suavizado): mistura a pose de pulo
    float land = 0;        // 1 -> 0 logo depois de pousar: os joelhos amortecem
};
static Player g_player;

// selecao -> nome -> modo de jogo -> (sala na rede: criar / procurar e entrar) -> jogo
enum GameState { STATE_SELECT, STATE_NAME, STATE_LOBBY, STATE_ROOM, STATE_JOIN, STATE_PLAY };
static GameState g_state = STATE_SELECT;
static const size_t MAX_NAME = 16;
static std::string g_name;          // nome digitado pelo jogador

// Menu aberto com Esc (na escolha de personagem e durante o jogo); o jogo fica parado enquanto estiver aberto.
enum MenuPage { MENU_NONE, MENU_MAIN, MENU_CONTROLS };
static MenuPage g_menu = MENU_NONE;
static int g_menuSel = 0;

static std::string displayName() {
    size_t end = g_name.find_last_not_of(' ');
    return end == std::string::npos ? "Jogador" : g_name.substr(0, end + 1);
}
static int   g_choice = 0;        // 0 = homem, 1 = mulher
// Vida: cinco coracoes. Cada tiro que acerta tira um (damagePlayer).
static const int MAX_HP = 5;
static int   g_hp = MAX_HP;
static float g_hurtTimer = 0.0f;  // > 0 logo depois de perder um coracao (anima o coracao perdido)
static const float HURT_TIME = 0.6f;

// Combate contra o bot: placar, morte e renascimento do jogador.
static int   g_scorePlayer = 0, g_scoreBot = 0;
static bool  g_playerDead = false;
static float g_deadTimer = 0.0f;        // segundos ate renascer
static float g_playerFall = 0.0f;       // 0 = em pe, 1 = caido
static float g_invuln = 0.0f;           // logo depois de levar um tiro nao leva outro
static const float INVULN_TIME = 0.35f;
static const float RESPAWN_TIME = 3.0f;
static float g_hurtDir[2] = { 0.0f, 0.0f };   // de onde veio o ultimo tiro que acertou o jogador (x, z)
static float g_hitMarker = 0.0f;              // > 0 logo depois de acertar o bot: marca na mira
static bool  g_fireLock = false;              // clique usado no menu nao vira tiro: so libera ao soltar o botao

static void netReportKill();       // partida em rede: avisa todos quem eliminou o jogador (definida com a rede)

static void damagePlayer(int hearts) {
    if (hearts <= 0 || g_hp <= 0 || g_invuln > 0.0f) return;
    g_hp = g_hp - hearts < 0 ? 0 : g_hp - hearts;
    g_hurtTimer = HURT_TIME;
    g_invuln = INVULN_TIME;
    if (g_hp == 0) {                                    // eliminado
        g_playerDead = true;
        g_deadTimer = RESPAWN_TIME;
        ++g_scoreBot;                                   // contra o bot: ponto do bot; na rede: mortes do jogador
        netReportKill();
    }
}

static bool  g_hasGun = false;   // item na mao: false = mao nua, true = arma
static float g_armBlend = 0.0f;   // 0..1: transicao da pose de bracos soltos para a pose com arma
static const int   MAG_SIZE = 30;          // tiros por carregador
static const float RELOAD_TIME = 5.0f;     // segundos para recarregar um carregador vazio; menos se ainda sobrar bala
static int   g_ammo = MAG_SIZE;
static float g_reloadTimer = 0.0f;         // > 0 enquanto recarrega
static float g_reloadTotal = RELOAD_TIME;  // duracao da recarga em andamento (proporcional as balas que faltavam)

static void startReload() {
    g_reloadTotal = RELOAD_TIME * (float)(MAG_SIZE - g_ammo) / (float)MAG_SIZE;
    g_reloadTimer = g_reloadTotal;
}
static float g_fireCd = 0.0f;     // tempo ate poder atirar de novo
static float g_flash = 0.0f;      // > 0 logo apos um tiro: clarao na boca do cano
static float g_aimHold = 0.0f;    // > 0 logo apos um tiro: o corpo fica virado para onde a camera mira
static float g_recoil = 0.0f;     // 1 -> 0 logo apos um tiro: coice da arma
static float g_shotHold = 0.0f, g_fireBlend = 0.0f;   // tempo restante "atirando" depois do ultimo tiro e a mistura 0..1
static const float SHOT_HOLD = 0.9f;   // por quanto tempo depois de um tiro a arma continua apontada (correndo)
// Correndo, a arma so fica apontada para a frente enquanto atira ou recarrega; senao volta para a posicao de carregar.
static void fireBlendStep(float& hold, float& blend, bool busy, float dt) {
    if (hold > 0.0f) hold -= dt;
    if (hold > 0.0f || busy) blend = blend + dt / 0.10f > 1.0f ? 1.0f : blend + dt / 0.10f;
    else                     blend = blend - dt / 0.30f < 0.0f ? 0.0f : blend - dt / 0.30f;
}
static float g_time = 0;

static const float SCALE = 1.7f;  // fator de tamanho do personagem (e de tudo que depende dele)

static float g_camYaw = 0.0f;     // 0 = camera atras do jogador olhando para -Z
static float g_camPitch = 0.35f;  // elevacao da camera
static const float CAM_DIST = 7.0f;   // nao escala junto: assim o personagem fica maior na tela

static const float PLAYER_RADIUS = 0.4f * SCALE;
static const float STEP_HEIGHT = 0.35f * SCALE;
static const float WALK_SPEED = 1.8f * SCALE;   // o ciclo de passos foi calibrado para estas velocidades
static const float RUN_SPEED = 5.0f * SCALE;
static const float PLAYER_HEIGHT = 1.8f * SCALE;

// aparencia escolhida para cada personagem
struct Look { int hair; int color; bool beard; bool outfit; int team; };   // team: colete 0 = normal, 1 = vermelho, 2 = azul
static Look g_look[2] = { { 2, 1, true, true, 0 }, { 3, 2, false, true, 0 } };   // homem, mulher
static const int NUM_HAIR = 5;
static const char* HAIR_NAMES[NUM_HAIR] = { "Nenhum", "Curto", "Repartido", "Longo", "Coque" };
static const int NUM_COLORS = 6;
static const char* COLOR_NAMES[NUM_COLORS] = { "Preto", "Castanho", "Loiro", "Ruivo", "Grisalho", "Rosa" };

// Partida em rede: sala com ate MAX_PLAYERS jogadores. O anfitriao escolhe o modo (todos contra todos ou duas equipes)
// e comeca a partida; vence quem (ou a equipe que) chegar primeiro a KILL_LIMIT abates.
enum NetMode { NET_NONE, NET_HOST, NET_JOIN };
static NetMode g_net = NET_NONE;
static const int MAX_PLAYERS = 8;
enum GameMode { MODE_FFA, MODE_TEAMS };
enum RoomPhase { PH_LOBBY, PH_PLAY, PH_END };    // sala de espera, partida, fim (placar final antes de voltar a sala)
static const char* const MODE_NAMES[2] = { "Todos contra todos", "Equipes" };
static const int KILL_LIMIT[2] = { 15, 25 };
static const float END_TIME = 8.0f;              // segundos mostrando o placar final
static int   g_mode = MODE_FFA, g_phase = PH_LOBBY;
static int   g_matchId = 0;                      // numero da partida atual (1..254), contado pelo anfitriao
static int   g_myMatch = -1;                     // partida em que este jogo esta (startMatch)
static int   g_winner = -1;                      // fim: numero do jogador (todos contra todos) ou da equipe
static int   g_killLimit = 0;                    // 0 = KILL_LIMIT do modo (o convidado recebe o do anfitriao)
static int   g_myId = 0, g_myTeam = 0;           // meu numero na sala (anfitriao = 0) e minha equipe (0 azul, 1 vermelha)
static int   g_myKills = 0, g_lastHitBy = -1;    // meus abates e quem me acertou por ultimo (para creditar a eliminacao)
static float g_endTimer = 0.0f;
struct FeedLine { std::string text; float t; int kind; };   // kind: 0 aviso, 1 eu eliminei, 2 fui eliminado
static std::vector<FeedLine> g_feed;             // eliminacoes, entradas e saidas (canto superior direito)
static const float COLOR_RGB[NUM_COLORS][3] = {
    { 0.20f, 0.18f, 0.17f }, { 0.62f, 0.40f, 0.22f }, { 1.00f, 0.82f, 0.45f },
    { 1.00f, 0.42f, 0.18f }, { 1.00f, 1.00f, 1.00f }, { 1.00f, 0.45f, 0.75f } };

// ---------------------------------------------------------------- utilidades

static unsigned int g_seed = 12345u;
static float frand() {
    g_seed = g_seed * 1664525u + 1013904223u;
    return (float)((g_seed >> 8) & 0xFFFF) / 65536.0f;
}

static float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

static void addBox(int kind, float x, float z, float hw, float hd, float h, float r, float g, float b) {
    if (g_numObs >= MAX_OBS) return;
    Obstacle& o = g_obs[g_numObs++];
    o.x = x; o.z = z; o.hw = hw; o.hd = hd; o.h = h; o.r = r; o.g = g; o.b = b; o.kind = kind;
}

// Altura do jogador = 3,06. Paredes e containers (4,6 a 5,2) escondem o personagem inteiro; caixas de ~3,5 tambem;
// as de ~2 sao baixas e dao para pular em cima (o pulo alcanca ~2,7).
static void initGalpao() {
    // paredes da sala
    const float wr = 0.42f, wg = 0.45f, wb = 0.50f;
    addBox(OB_WALL, 0.0f, -ROOM_Z - 0.5f, ROOM_X + 1.0f, 0.5f, ROOM_H, wr, wg, wb);
    addBox(OB_WALL, 0.0f,  ROOM_Z + 0.5f, ROOM_X + 1.0f, 0.5f, ROOM_H, wr, wg, wb);
    addBox(OB_WALL, -ROOM_X - 0.5f, 0.0f, 0.5f, ROOM_Z + 1.0f, ROOM_H, wr, wg, wb);
    addBox(OB_WALL,  ROOM_X + 0.5f, 0.0f, 0.5f, ROOM_Z + 1.0f, ROOM_H, wr, wg, wb);

    // pilares ate o teto
    const float pr = 0.52f, pg = 0.52f, pb = 0.55f;
    addBox(OB_PILLAR, -24.0f, -2.0f, 1.1f, 1.1f, ROOM_H, pr, pg, pb);
    addBox(OB_PILLAR,  24.0f, -2.0f, 1.1f, 1.1f, ROOM_H, pr, pg, pb);
    addBox(OB_PILLAR, -38.0f, -10.0f, 1.1f, 1.1f, ROOM_H, pr, pg, pb);
    addBox(OB_PILLAR,  38.0f,  10.0f, 1.1f, 1.1f, ROOM_H, pr, pg, pb);
    addBox(OB_PILLAR, -10.0f,  26.0f, 1.1f, 1.1f, ROOM_H, pr, pg, pb);
    addBox(OB_PILLAR,  12.0f, -28.0f, 1.1f, 1.1f, ROOM_H, pr, pg, pb);

    // muros de concreto no meio: formam corredores e cantos para se esconder
    const float cr = 0.56f, cg = 0.56f, cb = 0.53f;
    addBox(OB_COVER, -10.0f,  -8.0f, 9.0f, 0.6f, 4.6f, cr, cg, cb);
    addBox(OB_COVER,  10.0f, -14.0f, 9.0f, 0.6f, 4.6f, cr, cg, cb);
    addBox(OB_COVER, -18.0f,   6.0f, 0.6f, 8.0f, 4.6f, cr, cg, cb);
    addBox(OB_COVER,  16.0f,   4.0f, 0.6f, 9.0f, 4.6f, cr, cg, cb);
    addBox(OB_COVER,   0.0f,   8.0f, 7.0f, 0.6f, 4.6f, cr, cg, cb);
    addBox(OB_COVER, -30.0f,   8.0f, 5.0f, 0.6f, 4.6f, cr, cg, cb);
    addBox(OB_COVER,  30.0f,  -6.0f, 0.6f, 6.0f, 4.6f, cr, cg, cb);

    // containers
    addBox(OB_CONTAINER, -34.0f,  16.0f, 2.6f, 6.5f, 5.2f, 0.62f, 0.20f, 0.18f);
    addBox(OB_CONTAINER,  34.0f, -18.0f, 2.6f, 6.5f, 5.2f, 0.20f, 0.33f, 0.55f);
    addBox(OB_CONTAINER, -26.0f, -27.0f, 6.5f, 2.6f, 5.2f, 0.20f, 0.45f, 0.28f);
    addBox(OB_CONTAINER,  26.0f,  27.0f, 6.5f, 2.6f, 5.2f, 0.80f, 0.45f, 0.15f);

    // caixotes: altos (escondem) e baixos (da para pular em cima)
    const float kr = 0.55f, kg = 0.38f, kb = 0.20f;
    addBox(OB_CRATE,   3.0f,  -2.0f, 1.6f, 1.6f, 3.6f, kr, kg, kb);
    addBox(OB_CRATE,   6.0f,  -4.5f, 1.2f, 1.2f, 2.3f, kr * 0.9f, kg * 0.9f, kb * 0.9f);
    addBox(OB_CRATE,   0.0f,  -4.0f, 1.2f, 1.2f, 1.8f, kr * 1.1f, kg * 1.1f, kb * 1.1f);
    addBox(OB_CRATE,  -6.0f,   3.0f, 1.5f, 1.5f, 3.4f, kr, kg, kb);
    addBox(OB_CRATE,   8.0f,  11.0f, 1.5f, 1.5f, 3.4f, kr, kg, kb);
    addBox(OB_CRATE, -14.0f, -18.0f, 1.8f, 1.8f, 3.8f, kr, kg, kb);
    addBox(OB_CRATE,  20.0f,  -4.0f, 1.4f, 1.4f, 2.2f, kr * 0.9f, kg * 0.9f, kb * 0.9f);
    addBox(OB_CRATE, -22.0f,  16.0f, 1.6f, 1.6f, 3.2f, kr, kg, kb);
    addBox(OB_CRATE,  22.0f,  14.0f, 1.3f, 1.3f, 2.0f, kr * 1.1f, kg * 1.1f, kb * 1.1f);
    addBox(OB_CRATE,  36.0f,  24.0f, 1.8f, 1.8f, 3.8f, kr, kg, kb);
    addBox(OB_CRATE, -38.0f, -26.0f, 1.8f, 1.8f, 3.8f, kr, kg, kb);

    // barreiras baixas
    addBox(OB_BARRIER,  -6.0f, 17.0f, 3.0f, 0.6f, 1.5f, 0.60f, 0.60f, 0.58f);
    addBox(OB_BARRIER,   8.0f, 19.0f, 3.0f, 0.6f, 1.5f, 0.60f, 0.60f, 0.58f);
    addBox(OB_BARRIER,  20.0f, -22.0f, 0.6f, 3.0f, 1.5f, 0.60f, 0.60f, 0.58f);
    addBox(OB_BARRIER, -20.0f, -10.0f, 0.6f, 3.0f, 1.5f, 0.60f, 0.60f, 0.58f);

    // Anel de fora (o galpao cresceu para caber 8 jogadores; o miolo acima e a sala antiga). Cada peca tem a sua gemea
    // do outro lado do centro (x, z) -> (-x, -z), para as duas equipes terem o mesmo terreno.
    struct Extra { int kind; float x, z, hw, hd, h, r, g, b; };
    static const Extra extra[] = {
        // pilares
        { OB_PILLAR, -56.0f, -24.0f, 1.1f, 1.1f, ROOM_H, pr, pg, pb },
        { OB_PILLAR, -56.0f,  22.0f, 1.1f, 1.1f, ROOM_H, pr, pg, pb },
        // muros
        { OB_COVER, -56.0f,   0.0f, 0.6f, 9.0f, 4.6f, cr, cg, cb },
        { OB_COVER, -30.0f,  44.0f, 8.0f, 0.6f, 4.6f, cr, cg, cb },
        { OB_COVER,  32.0f,  42.0f, 0.6f, 6.0f, 4.6f, cr, cg, cb },
        { OB_COVER, -62.0f,  30.0f, 6.0f, 0.6f, 4.6f, cr, cg, cb },
        { OB_COVER,  -9.0f,  44.0f, 7.0f, 0.6f, 4.6f, cr, cg, cb },
        // containers
        { OB_CONTAINER, -66.0f, -38.0f, 2.6f, 6.5f, 5.2f, 0.62f, 0.20f, 0.18f },
        { OB_CONTAINER, -50.0f,  47.0f, 6.5f, 2.6f, 5.2f, 0.20f, 0.33f, 0.55f },
        { OB_CONTAINER, -64.0f,   8.0f, 2.6f, 6.5f, 5.2f, 0.80f, 0.45f, 0.15f },
        // caixotes
        { OB_CRATE, -48.0f, -44.0f, 1.8f, 1.8f, 3.8f, kr, kg, kb },
        { OB_CRATE, -20.0f, -46.0f, 1.5f, 1.5f, 3.4f, kr, kg, kb },
        { OB_CRATE, -52.0f,  14.0f, 1.6f, 1.6f, 3.2f, kr, kg, kb },
        { OB_CRATE, -68.0f,  44.0f, 1.8f, 1.8f, 3.8f, kr, kg, kb },
        { OB_CRATE, -45.5f,  30.0f, 1.3f, 1.3f, 2.0f, kr * 1.1f, kg * 1.1f, kb * 1.1f },
        { OB_CRATE,  14.0f,  40.0f, 1.4f, 1.4f, 2.2f, kr * 0.9f, kg * 0.9f, kb * 0.9f },
        { OB_CRATE, -70.0f, -14.0f, 1.5f, 1.5f, 3.4f, kr, kg, kb },
        { OB_CRATE,  60.0f,  34.0f, 1.2f, 1.2f, 1.8f, kr * 1.1f, kg * 1.1f, kb * 1.1f },
        // barreiras baixas
        { OB_BARRIER, -10.0f,  38.0f, 3.0f, 0.6f, 1.5f, 0.60f, 0.60f, 0.58f },
        { OB_BARRIER,  48.0f,   8.0f, 0.6f, 3.0f, 1.5f, 0.60f, 0.60f, 0.58f },
        { OB_BARRIER, -40.0f, -40.0f, 3.0f, 0.6f, 1.5f, 0.60f, 0.60f, 0.58f },
        { OB_BARRIER,  66.0f, -24.0f, 3.0f, 0.6f, 1.5f, 0.60f, 0.60f, 0.58f },
    };
    for (size_t i = 0; i < sizeof(extra) / sizeof(extra[0]); ++i) {
        const Extra& e = extra[i];
        addBox(e.kind, e.x, e.z, e.hw, e.hd, e.h, e.r, e.g, e.b);
        if (e.kind == OB_CONTAINER) addBox(e.kind, -e.x, -e.z, e.hw, e.hd, e.h, e.b, e.r + 0.15f, e.g);   // a gemea de outra cor
        else addBox(e.kind, -e.x, -e.z, e.hw, e.hd, e.h, e.r, e.g, e.b);
    }
}

// Pecas dos outros mapas: cada uma ganha a sua gemea do outro lado do centro, (x, z) -> (-x, -z), para as duas equipes
// terem o mesmo terreno. Todos os mapas tem o mesmo tamanho e deixam livres os mesmos pontos de nascimento.
struct Piece { int kind; float x, z, hw, hd, h, r, g, b; };
static void addPieces(const Piece* p, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        addBox(p[i].kind, p[i].x, p[i].z, p[i].hw, p[i].hd, p[i].h, p[i].r, p[i].g, p[i].b);
        addBox(p[i].kind, -p[i].x, -p[i].z, p[i].hw, p[i].hd, p[i].h, p[i].r, p[i].g, p[i].b);
    }
}

// Deserto, a ceu aberto: uma muralha baixa de rocha em volta (por cima dela se ve o ceu e os morros ao longe), blocos e
// torres de arenito, ruinas de adobe, containers enferrujados, caixotes e sacos de areia. Nas pecas de rocha e de adobe
// a cor e um tom que multiplica a textura.
static void initDeserto() {
    const float t = 3.0f;
    addBox(OB_WALL, 0.0f, -ROOM_Z - t, ROOM_X + 2.0f * t, t, DESERT_WALL_H, 1.0f, 1.0f, 1.0f);
    addBox(OB_WALL, 0.0f,  ROOM_Z + t, ROOM_X + 2.0f * t, t, DESERT_WALL_H, 1.0f, 1.0f, 1.0f);
    addBox(OB_WALL, -ROOM_X - t, 0.0f, t, ROOM_Z, DESERT_WALL_H, 1.0f, 1.0f, 1.0f);
    addBox(OB_WALL,  ROOM_X + t, 0.0f, t, ROOM_Z, DESERT_WALL_H, 1.0f, 1.0f, 1.0f);

    addBox(OB_ROCK, 0.0f, 0.0f, 6.5f, 4.5f, 7.5f, 1.0f, 0.97f, 0.94f);       // o rochedo do meio
    const float kr = 0.60f, kg = 0.42f, kb = 0.24f;                          // madeira desbotada pelo sol
    static const Piece pieces[] = {
        // blocos de arenito: os altos escondem, os baixos dao para pular em cima
        { OB_ROCK,  -8.5f,   2.5f, 2.5f, 3.0f, 3.6f, 0.95f, 0.95f, 0.95f },
        { OB_ROCK, -30.0f,  14.0f, 4.0f, 3.0f, 5.5f, 1.00f, 0.96f, 0.92f },
        { OB_ROCK, -27.0f,  36.0f, 3.0f, 4.5f, 6.5f, 0.96f, 0.98f, 1.00f },
        { OB_ROCK, -52.0f,   6.0f, 3.5f, 5.0f, 6.0f, 1.00f, 1.00f, 1.00f },
        { OB_ROCK, -60.0f,  34.0f, 4.5f, 3.0f, 4.6f, 0.94f, 0.92f, 0.90f },
        { OB_ROCK, -12.0f, -18.0f, 3.0f, 3.0f, 5.0f, 1.00f, 0.95f, 0.90f },
        { OB_ROCK, -44.0f, -22.0f, 5.0f, 3.5f, 7.0f, 0.97f, 0.97f, 0.97f },
        { OB_ROCK, -64.0f, -16.0f, 3.0f, 4.0f, 5.5f, 1.00f, 0.97f, 0.95f },
        { OB_ROCK, -34.0f, -40.0f, 4.0f, 3.0f, 6.0f, 0.95f, 0.95f, 0.98f },
        { OB_ROCK, -16.0f,   8.0f, 1.6f, 1.6f, 2.0f, 0.92f, 0.92f, 0.92f },
        { OB_ROCK, -36.0f,  22.0f, 1.8f, 1.4f, 1.8f, 0.96f, 0.93f, 0.90f },
        { OB_ROCK,  -6.0f, -34.0f, 1.5f, 1.8f, 2.2f, 0.94f, 0.94f, 0.94f },
        { OB_ROCK, -56.0f, -36.0f, 1.6f, 1.6f, 1.9f, 0.98f, 0.94f, 0.90f },
        { OB_ROCK, -28.0f,   0.0f, 1.4f, 1.4f, 1.7f, 0.92f, 0.92f, 0.95f },
        // torres de pedra
        { OB_PILLAR, -18.0f, -30.0f, 2.0f, 2.0f, 12.0f, 1.00f, 0.96f, 0.92f },
        { OB_PILLAR, -38.0f,   8.0f, 1.8f, 1.8f, 13.0f, 0.96f, 0.96f, 0.96f },
        { OB_PILLAR,  -8.0f,  42.0f, 1.7f, 1.7f, 11.0f, 1.00f, 0.98f, 0.95f },
        { OB_PILLAR, -50.0f, -48.0f, 2.0f, 2.0f, 12.0f, 0.95f, 0.93f, 0.92f },
        // ruinas de adobe: paredes em L e soltas
        { OB_COVER, -28.0f, -14.0f, 6.0f, 0.6f, 4.6f, 1.00f, 1.00f, 1.00f },
        { OB_COVER, -33.4f,  -9.4f, 0.6f, 4.0f, 3.4f, 0.96f, 0.96f, 0.96f },
        { OB_COVER, -50.0f,  24.0f, 0.6f, 6.0f, 4.6f, 1.00f, 1.00f, 1.00f },
        { OB_COVER, -44.8f,  29.4f, 4.6f, 0.6f, 3.4f, 0.96f, 0.96f, 0.96f },
        { OB_COVER,  13.0f,  24.0f, 5.0f, 0.6f, 4.6f, 1.00f, 0.98f, 0.96f },
        { OB_COVER,  22.0f,   6.0f, 0.6f, 5.0f, 4.6f, 1.00f, 1.00f, 1.00f },
        { OB_COVER, -60.0f,  -4.0f, 5.0f, 0.6f, 3.4f, 0.97f, 0.95f, 0.93f },
        { OB_COVER,  40.0f, -35.6f, 0.6f, 5.6f, 4.6f, 1.00f, 1.00f, 1.00f },
        // containers enferrujados
        { OB_CONTAINER, -14.0f,  31.0f, 6.5f, 2.6f, 5.2f, 0.55f, 0.30f, 0.18f },
        { OB_CONTAINER,  58.0f, -22.0f, 2.6f, 6.5f, 5.2f, 0.44f, 0.38f, 0.26f },
        // caixotes
        { OB_CRATE,   6.0f,  12.0f, 1.5f, 1.5f, 3.4f, kr, kg, kb },
        { OB_CRATE,  22.0f, -14.0f, 1.3f, 1.3f, 2.0f, kr * 1.1f, kg * 1.1f, kb * 1.1f },
        { OB_CRATE,  28.0f,  28.0f, 1.6f, 1.6f, 3.6f, kr, kg, kb },
        { OB_CRATE, -70.0f,   6.0f, 1.5f, 1.5f, 3.2f, kr, kg, kb },
        { OB_CRATE,  46.0f, -46.0f, 1.8f, 1.8f, 3.8f, kr, kg, kb },
        { OB_CRATE,  -6.3f,  31.0f, 1.2f, 1.2f, 2.2f, kr * 0.9f, kg * 0.9f, kb * 0.9f },
        // sacos de areia
        { OB_BARRIER, -10.0f,  12.0f, 3.0f, 0.6f, 1.5f, 1.0f, 1.0f, 1.0f },
        { OB_BARRIER,  26.0f, -22.0f, 0.6f, 3.0f, 1.5f, 1.0f, 1.0f, 1.0f },
        { OB_BARRIER, -46.0f,  -8.0f, 3.0f, 0.6f, 1.5f, 1.0f, 1.0f, 1.0f },
        { OB_BARRIER,  14.0f,  40.0f, 3.0f, 0.6f, 1.5f, 1.0f, 1.0f, 1.0f },
    };
    addPieces(pieces, sizeof(pieces) / sizeof(pieces[0]));
}

// Sala branca: paredes, teto, piso e blocos, tudo da mesma cor. Os blocos so se percebem de perto, pelas arestas.
static void initBranco() {
    const float w = 1.0f;
    addBox(OB_WALL, 0.0f, -ROOM_Z - 0.5f, ROOM_X + 1.0f, 0.5f, ROOM_H, w, w, w);
    addBox(OB_WALL, 0.0f,  ROOM_Z + 0.5f, ROOM_X + 1.0f, 0.5f, ROOM_H, w, w, w);
    addBox(OB_WALL, -ROOM_X - 0.5f, 0.0f, 0.5f, ROOM_Z + 1.0f, ROOM_H, w, w, w);
    addBox(OB_WALL,  ROOM_X + 0.5f, 0.0f, 0.5f, ROOM_Z + 1.0f, ROOM_H, w, w, w);

    addBox(OB_CRATE, 0.0f, 0.0f, 3.0f, 3.0f, 3.0f, w, w, w);
    static const Piece pieces[] = {
        // colunas ate o teto
        { OB_PILLAR,  -8.0f,  -8.0f, 1.5f, 1.5f, ROOM_H, w, w, w },
        { OB_PILLAR,  -8.0f,   8.0f, 1.5f, 1.5f, ROOM_H, w, w, w },
        { OB_PILLAR, -38.0f, -28.0f, 1.2f, 1.2f, ROOM_H, w, w, w },
        { OB_PILLAR, -52.0f,  40.0f, 1.2f, 1.2f, ROOM_H, w, w, w },
        { OB_PILLAR, -20.0f,  12.0f, 1.2f, 1.2f, ROOM_H, w, w, w },
        // paredes soltas
        { OB_COVER, -24.0f,   0.0f, 0.6f, 10.0f, 4.6f, w, w, w },
        { OB_COVER,   0.0f, -16.0f, 10.0f, 0.6f, 4.6f, w, w, w },
        { OB_COVER, -44.0f, -18.0f, 8.0f, 0.6f, 4.6f, w, w, w },
        { OB_COVER, -44.0f,  18.0f, 0.6f, 8.0f, 4.6f, w, w, w },
        { OB_COVER, -62.0f,   0.0f, 0.6f, 12.0f, 4.6f, w, w, w },
        { OB_COVER, -30.0f, -36.0f, 0.6f, 7.0f, 4.6f, w, w, w },
        { OB_COVER, -14.0f,  36.0f, 9.0f, 0.6f, 4.6f, w, w, w },
        // cubos altos (escondem)
        { OB_CRATE, -36.0f,   2.0f, 2.0f, 2.0f, 4.0f, w, w, w },
        { OB_CRATE, -52.0f, -34.0f, 2.0f, 2.0f, 4.0f, w, w, w },
        { OB_CRATE, -16.0f, -26.0f, 2.0f, 2.0f, 4.0f, w, w, w },
        { OB_CRATE, -66.0f, -40.0f, 2.0f, 2.0f, 4.0f, w, w, w },
        { OB_CRATE, -70.0f,  22.0f, 2.0f, 2.0f, 4.0f, w, w, w },
        // cubos baixos (da para pular em cima)
        { OB_CRATE, -14.0f,   4.0f, 1.5f, 1.5f, 2.0f, w, w, w },
        { OB_CRATE, -30.0f,  22.0f, 1.5f, 1.5f, 2.0f, w, w, w },
        { OB_CRATE, -50.0f,   8.0f, 1.5f, 1.5f, 2.2f, w, w, w },
        { OB_CRATE, -22.0f, -44.0f, 1.5f, 1.5f, 2.0f, w, w, w },
        { OB_CRATE, -56.0f, -16.0f, 1.5f, 1.5f, 2.0f, w, w, w },
        // blocos compridos
        { OB_CONTAINER, -40.0f,  34.0f, 6.5f, 2.6f, 5.2f, w, w, w },
        { OB_CONTAINER, -10.0f, -38.0f, 2.6f, 5.0f, 5.2f, w, w, w },
    };
    addPieces(pieces, sizeof(pieces) / sizeof(pieces[0]));
}

// Monta as caixas do mapa que esta em g_worldMap.
static void initWorld() {
    g_numObs = 0;
    g_ceil = ROOM_H;
    if (g_worldMap == MAP_DESERTO) { initDeserto(); g_ceil = 1.0e4f; }      // sem teto
    else if (g_worldMap == MAP_BRANCO) initBranco();
    else initGalpao();
}

// Pontos de nascimento, junto as paredes: os 9 primeiros no lado sul (z > 0), os outros 9 sao os mesmos no lado norte.
static const int NUM_SPAWNS = 18;
static const float SPAWNS[NUM_SPAWNS][2] = {
    { 4.0f, 50.0f }, { -26.0f, 51.0f }, { 26.0f, 51.0f }, { -42.0f, 51.0f }, { 42.0f, 51.0f }, { -62.0f, 51.0f }, { 60.0f, 51.0f },
    { -71.0f, 36.0f }, { 71.0f, 23.0f },
    { -4.0f, -50.0f }, { 26.0f, -51.0f }, { -26.0f, -51.0f }, { 42.0f, -51.0f }, { -42.0f, -51.0f }, { 62.0f, -51.0f }, { -60.0f, -51.0f },
    { 71.0f, -36.0f }, { -71.0f, -23.0f } };

// ---------------------------------------------------------------- modelo com esqueleto (.mdl)

struct Mat4 { float m[16]; };   // column-major, como o OpenGL

static Mat4 mul(const Mat4& a, const Mat4& b) {
    Mat4 r;
    for (int c = 0; c < 4; ++c)
        for (int row = 0; row < 4; ++row) {
            float s = 0;
            for (int k = 0; k < 4; ++k) s += a.m[k * 4 + row] * b.m[c * 4 + k];
            r.m[c * 4 + row] = s;
        }
    return r;
}

struct Vertex { float p[3], n[3], uv[2]; unsigned char j[4]; float w[4]; };
// type: 0 = corpo, 1 = sobrancelha, 2 = cabelo (id = estilo), 3 = barba
struct Part   { unsigned first, count; int tex, type, id; };
struct Joint  { char name[33]; int parent; Mat4 rest, inv; };

// Medidas de uma perna na pose de repouso (espaco do modelo, perna esticada para baixo).
// Angulos "alpha" medem a direcao de um osso no plano YZ: 0 = para baixo, positivo = para tras.
struct LegRig {
    float hipY = 0, hipZ = 0;           // articulacao do quadril
    float ankleY = 0, ankleZ = 0;       // tornozelo
    float ballY = 0, ballZ = 0;         // ponta do pe, relativa ao tornozelo
    float L1 = 1, L2 = 1;               // coxa e canela
    float a0t = 0, a0c = 0;             // direcao de repouso da coxa e da canela
};

struct Model {
    bool loaded = false;
    std::vector<Vertex>   verts;
    std::vector<unsigned> idx;
    std::vector<Joint>    joints;
    std::vector<Part>     parts;
    std::vector<GLuint>   tex;
    std::vector<float>    skinP, skinN;     // vertices ja deformados pelo esqueleto
    std::vector<Mat4>     skinM;
    std::vector<Mat4>     G, restG;         // pose atual e pose de repouso de cada osso (espaco do modelo)
    std::vector<float>    ax, ay, az;       // rotacao extra de cada osso (eixos X, Y e Z do espaco do modelo)
    float scale = 1, minY = 0;

    int root = -1, spine1 = -1, spine2 = -1, spine3 = -1, neck = -1, head = -1;
    int thigh[2] = { -1, -1 }, calf[2] = { -1, -1 }, foot[2] = { -1, -1 }, ball[2] = { -1, -1 };
    int upper[2] = { -1, -1 }, lower[2] = { -1, -1 }, hand[2] = { -1, -1 };   // 0 = esquerda (+X), 1 = direita
    LegRig rig[2];
    struct Finger { int joint, level, side; bool thumb; };
    std::vector<Finger> fingers;
    // posicao da arma na ultima pose (coice e recarga): poseModel calcula, drawWeapon e muzzleWorld usam
    float wKick = 0, wTilt = 0, wRoll = 0, magDrop = 0;
    float wPos[3] = { 0, 0, 0 }, wYaw = 0;      // onde a coronha apoia (relativo ao meio dos ombros) e giro da arma
    bool  magHidden = false;
};

static std::string g_baseDir;

// ---- roupa tatica gerada por codigo
// Cada peca e uma "casca": copia dos triangulos do corpo naquela regiao, empurrada um pouco para fora ao longo
// das normais, com os mesmos pesos de esqueleto (por isso acompanha todos os movimentos) e uma cor lisa.
enum ClothPiece { CL_SHIRT, CL_PANTS, CL_BOOTS, CL_GLOVES, CL_VEST, CL_BELT, CL_KNEEPADS, CL_POCKETS, NUM_CLOTH };
static const float CLOTH_COLOR[NUM_CLOTH][3] = {
    { 0.10f, 0.11f, 0.12f },    // camisa de manga comprida: preto
    { 0.15f, 0.16f, 0.14f },    // calca cargo: grafite esverdeado
    { 0.06f, 0.06f, 0.06f },    // botas
    { 0.07f, 0.07f, 0.08f },    // luvas
    { 0.20f, 0.21f, 0.19f },    // colete
    { 0.05f, 0.05f, 0.05f },    // cinto
    { 0.08f, 0.08f, 0.09f },    // joelheiras
    { 0.19f, 0.20f, 0.17f },    // bolsos da calca
};
// quanto cada peca fica afastada do corpo, em fracao da altura do modelo
static const float CLOTH_OFFSET[NUM_CLOTH] = { 0.0030f, 0.0035f, 0.0070f, 0.0045f, 0.0085f, 0.0070f, 0.0080f, 0.0070f };

enum BodyGroup { BG_OTHER, BG_TORSO, BG_PELVIS, BG_ARM, BG_HAND, BG_LEG, BG_FOOT, BG_HEAD };

static BodyGroup bodyGroupOf(const char* n) {
    if (!strncmp(n, "spine", 5)) return BG_TORSO;
    if (!strcmp(n, "pelvis") || !strcmp(n, "root")) return BG_PELVIS;
    if (!strncmp(n, "clavicle", 8) || !strncmp(n, "upperarm", 8) || !strncmp(n, "lowerarm", 8)) return BG_ARM;
    if (!strncmp(n, "hand", 4) || !strncmp(n, "index", 5) || !strncmp(n, "middle", 6) || !strncmp(n, "pinky", 5) ||
        !strncmp(n, "ring", 4) || !strncmp(n, "thumb", 5)) return BG_HAND;
    if (!strncmp(n, "thigh", 5) || !strncmp(n, "calf", 4)) return BG_LEG;
    if (!strncmp(n, "foot", 4) || !strncmp(n, "ball", 4)) return BG_FOOT;
    if (!strncmp(n, "neck", 4) || !strcmp(n, "Head")) return BG_HEAD;
    return BG_OTHER;
}

static int jointByName(const Model& m, const char* name) {
    for (size_t i = 0; i < m.joints.size(); ++i) if (!strcmp(m.joints[i].name, name)) return (int)i;
    return -1;
}

static void buildOutfit(Model& m, float height) {
    // o corpo e a maior parte "normal" (as outras sao olhos e sobrancelhas)
    int body = -1;
    for (size_t i = 0; i < m.parts.size(); ++i)
        if (m.parts[i].type == 0 && (body < 0 || m.parts[i].count > m.parts[(size_t)body].count)) body = (int)i;
    int pelvis = jointByName(m, "pelvis");
    if (body < 0 || pelvis < 0 || m.neck < 0) return;

    const float H = height;
    const float pelvisY = m.restG[(size_t)pelvis].m[13];
    const float neckY = m.restG[(size_t)m.neck].m[13];
    const float kneeY = m.restG[(size_t)m.calf[0]].m[13];
    const float ankleY = m.restG[(size_t)m.foot[0]].m[13];
    const float wristAbsX = std::fabs(m.restG[(size_t)m.hand[0]].m[12]);
    const float TORSO_HALF = 0.105f * H;            // a partir daqui, na altura dos ombros, ja e braco (pose em T)

    size_t nV = m.verts.size();
    std::vector<BodyGroup> grp(nV, BG_OTHER);
    for (size_t v = 0; v < nV; ++v) {
        int best = 0;
        for (int k = 1; k < 4; ++k) if (m.verts[v].w[k] > m.verts[v].w[best]) best = k;
        grp[v] = bodyGroupOf(m.joints[m.verts[v].j[best]].name);
    }

    const Part bodyPart = m.parts[(size_t)body];
    std::vector<unsigned> bodyIdx(m.idx.begin() + bodyPart.first, m.idx.begin() + bodyPart.first + bodyPart.count);

    for (int piece = 0; piece < NUM_CLOTH; ++piece) {
        std::vector<int> remap(nV, -1);
        unsigned firstIdx = (unsigned)m.idx.size();
        for (size_t t = 0; t + 2 < bodyIdx.size(); t += 3) {
            bool ok = true;
            for (int c = 0; c < 3 && ok; ++c) {
                unsigned v = bodyIdx[t + (size_t)c];
                const Vertex& vt = m.verts[v];
                float x = vt.p[0], y = vt.p[1], ax = std::fabs(x);
                BodyGroup g = grp[v];
                switch (piece) {
                case CL_SHIRT:
                    // as regioes sao cortadas por posicao (e nao pelo peso dos ossos) para as bordas ficarem limpas
                    if (g != BG_TORSO && g != BG_PELVIS && g != BG_ARM) ok = false;
                    else if (ax >= TORSO_HALF) ok = ax < wristAbsX - 0.01f * H && y > pelvisY + 0.13f * H && y < neckY + 0.05f * H;   // manga
                    else {
                        float tt = clampf((ax - 0.06f * H) / (0.04f * H), 0.0f, 1.0f);   // o decote sobe em direcao ao ombro
                        ok = y > pelvisY - 0.015f * H && y < neckY - 0.012f * H + tt * tt * (3.0f - 2.0f * tt) * 0.045f * H;
                    }
                    break;
                case CL_PANTS:
                    ok = (g == BG_PELVIS || g == BG_TORSO || g == BG_LEG) && y < pelvisY + 0.05f * H && y > ankleY + 0.085f * H;
                    break;
                case CL_BOOTS:
                    ok = g == BG_FOOT || (g == BG_LEG && y < ankleY + 0.11f * H);
                    break;
                case CL_GLOVES:
                    ok = g == BG_HAND;
                    break;
                case CL_VEST:
                    ok = (g == BG_TORSO || g == BG_PELVIS || g == BG_ARM) && ax < 0.092f * H &&
                         y > pelvisY + 0.06f * H && y < neckY - 0.04f * H;
                    break;
                case CL_BELT:
                    ok = g != BG_ARM && g != BG_HAND && g != BG_HEAD && g != BG_FOOT && y > pelvisY && y < pelvisY + 0.035f * H;
                    break;
                case CL_KNEEPADS:
                    ok = g == BG_LEG && std::fabs(y - kneeY) < 0.045f * H && vt.n[2] > 0.35f;
                    break;
                case CL_POCKETS:
                    ok = g == BG_LEG && y > kneeY + 0.06f * H && y < kneeY + 0.15f * H && x * vt.n[0] > 0.0f &&
                         std::fabs(vt.n[0]) > 0.5f;
                    break;
                }
            }
            if (!ok) continue;
            for (int c = 0; c < 3; ++c) {
                unsigned v = bodyIdx[t + (size_t)c];
                if (remap[v] < 0) {
                    Vertex nv = m.verts[v];
                    float off = CLOTH_OFFSET[piece] * H;
                    for (int k = 0; k < 3; ++k) nv.p[k] += nv.n[k] * off;
                    remap[v] = (int)m.verts.size();
                    m.verts.push_back(nv);
                }
                m.idx.push_back((unsigned)remap[v]);
            }
        }
        unsigned cnt = (unsigned)m.idx.size() - firstIdx;
        if (cnt) {
            Part pt; pt.first = firstIdx; pt.count = cnt; pt.tex = -1; pt.type = 4; pt.id = piece;
            m.parts.push_back(pt);
        }
    }
}

static bool loadModel(Model& m, const std::string& path) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    std::vector<unsigned char> buf((size_t)size);
    size_t got = fread(buf.data(), 1, buf.size(), f);
    fclose(f);
    if (got != buf.size()) return false;

    const unsigned char* p = buf.data();
    const unsigned char* end = p + buf.size();
    #define NEED(n) if ((size_t)(end - p) < (size_t)(n)) return false
    NEED(24);
    if (memcmp(p, "MDL2", 4) != 0) return false;
    p += 4;
    unsigned hdr[5];
    memcpy(hdr, p, 20); p += 20;
    unsigned nV = hdr[0], nI = hdr[1], nJ = hdr[2], nP = hdr[3], nT = hdr[4];

    m.verts.resize(nV);
    NEED(nV * sizeof(Vertex));
    for (unsigned i = 0; i < nV; ++i) {
        Vertex& v = m.verts[i];
        memcpy(v.p, p, 12); p += 12;
        memcpy(v.n, p, 12); p += 12;
        memcpy(v.uv, p, 8); p += 8;
        memcpy(v.j, p, 4); p += 4;
        memcpy(v.w, p, 16); p += 16;
    }
    m.idx.resize(nI);
    NEED(nI * 4);
    memcpy(m.idx.data(), p, nI * 4); p += nI * 4;

    m.joints.resize(nJ);
    NEED(nJ * 164);
    for (unsigned i = 0; i < nJ; ++i) {
        Joint& j = m.joints[i];
        memcpy(j.name, p, 32); j.name[32] = 0; p += 32;
        int par;
        memcpy(&par, p, 4); p += 4;
        j.parent = par;
        memcpy(j.rest.m, p, 64); p += 64;
        memcpy(j.inv.m, p, 64); p += 64;
    }
    m.parts.resize(nP);
    NEED(nP * 20);
    for (unsigned i = 0; i < nP; ++i) {
        unsigned a[2]; int t[3];
        memcpy(a, p, 8); p += 8;
        memcpy(t, p, 12); p += 12;
        m.parts[i].first = a[0]; m.parts[i].count = a[1];
        m.parts[i].tex = t[0]; m.parts[i].type = t[1]; m.parts[i].id = t[2];
    }
    m.tex.resize(nT);
    for (unsigned i = 0; i < nT; ++i) {
        NEED(8);
        unsigned wh[2];
        memcpy(wh, p, 8); p += 8;
        NEED((size_t)wh[0] * wh[1] * 4);
        glGenTextures(1, &m.tex[i]);
        glBindTexture(GL_TEXTURE_2D, m.tex[i]);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_GENERATE_MIPMAP, GL_TRUE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, (GLsizei)wh[0], (GLsizei)wh[1], 0, GL_RGBA, GL_UNSIGNED_BYTE, p);
        p += (size_t)wh[0] * wh[1] * 4;
    }
    #undef NEED

    float lo = 1e9f, hi = -1e9f;
    for (unsigned i = 0; i < nV; ++i) {
        lo = m.verts[i].p[1] < lo ? m.verts[i].p[1] : lo;
        hi = m.verts[i].p[1] > hi ? m.verts[i].p[1] : hi;
    }
    m.minY = lo;
    m.scale = PLAYER_HEIGHT / (hi - lo);

    m.skinP.assign(nV * 3, 0.0f);
    m.skinN.assign(nV * 3, 0.0f);
    m.skinM.resize(nJ);
    m.ax.assign(nJ, 0.0f);
    m.ay.assign(nJ, 0.0f);
    m.az.assign(nJ, 0.0f);

    for (unsigned i = 0; i < nJ; ++i) {
        const char* n = m.joints[i].name;
        int ii = (int)i;
        if (!strcmp(n, "root")) m.root = ii;
        else if (!strcmp(n, "spine_01")) m.spine1 = ii;
        else if (!strcmp(n, "spine_02")) m.spine2 = ii;
        else if (!strcmp(n, "spine_03")) m.spine3 = ii;
        else if (!strcmp(n, "neck_01")) m.neck = ii;
        else if (!strcmp(n, "Head")) m.head = ii;
        static const char* fingerNames[5] = { "index", "middle", "ring", "pinky", "thumb" };
        for (int fi = 0; fi < 5; ++fi) {        // falanges "index_01_l", ... (as pontas "_04_leaf_" nao entram)
            size_t len = strlen(fingerNames[fi]);
            if (!strncmp(n, fingerNames[fi], len) && n[len] == '_' && n[len + 1] == '0' && n[len + 2] >= '1' &&
                n[len + 2] <= '3' && n[len + 3] == '_' && (n[len + 4] == 'l' || n[len + 4] == 'r') && n[len + 5] == 0) {
                Model::Finger f = { ii, n[len + 2] - '0', n[len + 4] == 'l' ? 0 : 1, fi == 4 };
                m.fingers.push_back(f);
            }
        }
        for (int sd = 0; sd < 2; ++sd) {
            const char sfx = sd == 0 ? 'l' : 'r';
            char want[32];
            #define MATCH(base, arr) snprintf(want, sizeof(want), base "_%c", sfx); if (!strcmp(n, want)) m.arr[sd] = ii
            MATCH("thigh", thigh); MATCH("calf", calf); MATCH("foot", foot); MATCH("ball", ball);
            MATCH("upperarm", upper); MATCH("lowerarm", lower); MATCH("hand", hand);
            #undef MATCH
        }
    }
    m.restG.resize(nJ);
    m.G.resize(nJ);
    for (unsigned i = 0; i < nJ; ++i) {
        const Joint& j = m.joints[i];
        m.restG[i] = j.parent < 0 ? j.rest : mul(m.restG[(size_t)j.parent], j.rest);
    }
    m.G = m.restG;

    for (int sd = 0; sd < 2; ++sd) {
        if (m.thigh[sd] < 0 || m.calf[sd] < 0 || m.foot[sd] < 0 || m.ball[sd] < 0 ||
            m.upper[sd] < 0 || m.lower[sd] < 0 || m.hand[sd] < 0) return false;
        const float* hip = &m.restG[(size_t)m.thigh[sd]].m[12];
        const float* knee = &m.restG[(size_t)m.calf[sd]].m[12];
        const float* ankle = &m.restG[(size_t)m.foot[sd]].m[12];
        const float* ball = &m.restG[(size_t)m.ball[sd]].m[12];
        LegRig& r = m.rig[sd];
        r.hipY = hip[1];     r.hipZ = hip[2];
        r.ankleY = ankle[1]; r.ankleZ = ankle[2];
        r.ballY = ball[1] - ankle[1]; r.ballZ = ball[2] - ankle[2];
        r.L1 = std::sqrt((knee[1] - hip[1]) * (knee[1] - hip[1]) + (knee[2] - hip[2]) * (knee[2] - hip[2]));
        r.L2 = std::sqrt((ankle[1] - knee[1]) * (ankle[1] - knee[1]) + (ankle[2] - knee[2]) * (ankle[2] - knee[2]));
        r.a0t = std::atan2(-(knee[2] - hip[2]), -(knee[1] - hip[1]));
        r.a0c = std::atan2(-(ankle[2] - knee[2]), -(ankle[1] - knee[1]));
    }
    buildOutfit(m, hi - lo);
    m.skinP.assign(m.verts.size() * 3, 0.0f);          // a roupa acrescentou vertices
    m.skinN.assign(m.verts.size() * 3, 0.0f);
    m.loaded = true;
    return true;
}

// Rotaciona as 3 primeiras colunas (eixos) de um osso em torno do eixo X, Y ou Z do mundo do modelo.
// A posicao do osso (coluna 3) nao muda: a rotacao e feita em torno da propria articulacao.
static void rotateBoneX(Mat4& g, float a) {
    float c = std::cos(a), s = std::sin(a);
    for (int col = 0; col < 3; ++col) {
        float* v = &g.m[col * 4];
        float y = v[1], z = v[2];
        v[1] = y * c - z * s;
        v[2] = y * s + z * c;
    }
}
static void rotateBoneY(Mat4& g, float a) {
    float c = std::cos(a), s = std::sin(a);
    for (int col = 0; col < 3; ++col) {
        float* v = &g.m[col * 4];
        float x = v[0], z = v[2];
        v[0] = x * c + z * s;
        v[2] = -x * s + z * c;
    }
}
static void rotateBoneZ(Mat4& g, float a) {
    float c = std::cos(a), s = std::sin(a);
    for (int col = 0; col < 3; ++col) {
        float* v = &g.m[col * 4];
        float x = v[0], y = v[1];
        v[0] = x * c - y * s;
        v[1] = x * s + y * c;
    }
}

static float smooth01(float t) { t = clampf(t, 0.0f, 1.0f); return t * t * (3.0f - 2.0f * t); }

// minimo suave entre a e b (k = largura da suavizacao)
static float softMin(float a, float b, float k) {
    float h = clampf(0.5f + 0.5f * (b - a) / k, 0.0f, 1.0f);
    return b * (1.0f - h) + a * h - k * h * (1.0f - h);
}

// ---- ciclo de passos
// Dois estilos que se misturam pela velocidade s (m/s do modelo): caminhada calma (r = 0) e corrida (r = 1).
// Cada pe apoiado anda para tras na velocidade do corpo, entao nao desliza no chao. As curvas do pe e do
// quadril passam por um filtro de Fourier (poucos harmonicos) para nao ter quinas: o movimento fica fluido.

static float lerpf(float a, float b, float t) { return a + (b - a) * t; }

struct Gait {
    float f;      // ciclos por segundo
    float beta;   // fracao do ciclo com o pe no chao
    float S;      // meia passada: o pe vai de +S a -S em relacao ao quadril durante o apoio
    float lift;   // altura maxima do pe no balanco
    float w;      // 0 (parado) a 1 (em movimento): atenua tudo perto de s = 0
    float r;      // 0 = caminhada, 1 = corrida
};

static float gaitBlend(float s) { return smooth01((s - 2.4f) / 2.0f); }

static float gaitCadence(float s) { return lerpf(0.75f + 0.30f * s, 1.10f + 0.20f * s, gaitBlend(s)); }

static Gait gaitFor(float s) {
    Gait g;
    g.r = gaitBlend(s);
    g.f = gaitCadence(s);
    g.beta = lerpf(0.58f, 0.36f, g.r);
    g.S = s * g.beta / (2.0f * g.f);
    g.w = smooth01(s);
    g.lift = lerpf(0.06f, 0.19f, g.r) * g.w;
    return g;
}

// Inclinacao do pe ao longo do ciclo (+ = ponta para cima): toca com o calcanhar, assenta, rola ate a ponta
// do pe, solta e levanta a ponta para o proximo passo.
static float footPitch(float u, const Gait& g) {
    float touch = lerpf(0.25f, 0.12f, g.r), off = lerpf(-0.50f, -0.60f, g.r);
    const float k[5][2] = { { 0.0f, touch }, { 0.55f * g.beta, 0.0f }, { g.beta, off },
                            { g.beta + 0.45f * (1.0f - g.beta), off * 0.3f }, { 1.0f, touch } };
    for (int i = 0; i < 4; ++i)
        if (u <= k[i + 1][0]) {
            float t = (u - k[i][0]) / (k[i + 1][0] - k[i][0]);
            return k[i][1] + (k[i + 1][1] - k[i][1]) * smooth01(t);
        }
    return k[4][1];
}

struct FootTarget { float dz, dy, pitch; };   // deslocamento do tornozelo e inclinacao do pe

static FootTarget footRaw(float u, const Gait& g, float s, const LegRig& r) {
    FootTarget t;
    float pitch = footPitch(u, g) * g.w;
    if (u < g.beta) {                                   // apoio: pe preso ao chao, indo para tras
        t.dz = g.S * (1.0f - 2.0f * u / g.beta);
        t.dy = 0.0f;
    } else {                                            // balanco: Hermite cubico, com a velocidade do chao nas pontas
        float x = (u - g.beta) / (1.0f - g.beta);
        float m = -s * (1.0f - g.beta) / g.f;
        float x2 = x * x, x3 = x2 * x;
        t.dz = (2 * x3 - 3 * x2 + 1) * -g.S + (x3 - 2 * x2 + x) * m + (-2 * x3 + 3 * x2) * g.S + (x3 - x2) * m;
        t.dy = g.lift * std::pow(std::sin(PI * x), 1.3f);
    }
    t.dz += (r.hipZ - r.ankleZ) * 0.8f * g.w;           // centra a passada embaixo do quadril (menos queda do quadril)
    // calcanhar sobe girando em torno da ponta do pe (que continua no chao)
    float th = pitch < 0.0f ? -pitch : 0.0f;
    t.dy += r.ballY * (1.0f - std::cos(th)) + r.ballZ * std::sin(th);
    t.pitch = pitch;
    return t;
}

// ---- serie de Fourier (GH harmonicos) de uma curva periodica amostrada em GN pontos
static const int GN = 64, GH = 8;
struct Harm { float a0, a[GH], b[GH]; };

static void harmFit(Harm& h, const float* x) {
    static float ct[GH][GN], st[GH][GN];
    static bool init = false;
    if (!init) {
        for (int k = 0; k < GH; ++k)
            for (int i = 0; i < GN; ++i) {
                ct[k][i] = std::cos(2.0f * PI * (k + 1) * i / GN);
                st[k][i] = std::sin(2.0f * PI * (k + 1) * i / GN);
            }
        init = true;
    }
    float mean = 0.0f;
    for (int i = 0; i < GN; ++i) mean += x[i];
    h.a0 = mean / GN;
    for (int k = 0; k < GH; ++k) {
        float a = 0.0f, b = 0.0f;
        for (int i = 0; i < GN; ++i) { a += x[i] * ct[k][i]; b += x[i] * st[k][i]; }
        h.a[k] = 2.0f * a / GN;
        h.b[k] = 2.0f * b / GN;
    }
}

static float harmEval(const Harm& h, float u) {
    float v = h.a0;
    for (int k = 0; k < GH; ++k) {
        float ang = 2.0f * PI * (k + 1) * u;
        v += h.a[k] * std::cos(ang) + h.b[k] * std::sin(ang);
    }
    return v;
}

struct GaitCurve { Harm dz, dy, pitch, hip; };

static float wrap01(float u) { return u - std::floor(u); }

static void buildGaitCurve(GaitCurve& c, const Gait& g, float s, const LegRig& r, float restHip) {
    float dz[GN], dy[GN], pt[GN], hip[GN];
    for (int i = 0; i < GN; ++i) {
        FootTarget t = footRaw((float)i / GN, g, s, r);
        dz[i] = t.dz; dy[i] = t.dy; pt[i] = t.pitch;
    }
    harmFit(c.dz, dz); harmFit(c.dy, dy); harmFit(c.pitch, pt);

    // quadril: agacha um pouco, quica (caminhada: mais alto no meio do apoio; corrida: mais baixo) e nao pode
    // ficar acima do que as pernas alcancam
    float crouch = (0.02f + 0.04f * g.r) * g.w + 0.012f * (1.0f - g.w);
    float pp = lerpf(0.03f, 0.07f, g.r) * g.w;
    float cs = 2.0f * g.r - 1.0f;
    float reach = lerpf(0.985f, 0.95f, g.r) * (r.L1 + r.L2);
    for (int i = 0; i < GN; ++i) {
        float u0 = (float)i / GN;
        float h = restHip - crouch - pp * 0.5f * (1.0f + cs * std::cos(4.0f * PI * (u0 - g.beta * 0.5f)));
        for (int sd = 0; sd < 2; ++sd) {
            float u = wrap01(u0 + 0.5f * sd);
            float zf = (r.ankleZ - r.hipZ) + harmEval(c.dz, u);
            float bound = r.ankleY + std::max(0.0f, harmEval(c.dy, u)) + std::sqrt(std::max(reach * reach - zf * zf, 0.0f));
            h = softMin(h, bound, 0.03f);
        }
        hip[i] = h;
    }
    harmFit(c.hip, hip);
}

// Cinematica inversa de 2 ossos no plano YZ: acha os angulos da coxa, joelho e pe para o tornozelo
// chegar em (tAy, tAz) com o quadril em altura hy.
static void solveLeg(const LegRig& r, float hy, float tAy, float tAz, float pitch, float& a1, float& a2, float& a3) {
    float vy = tAy - hy, vz = tAz - r.hipZ;
    float d = std::sqrt(vy * vy + vz * vz);
    d = clampf(d, std::fabs(r.L1 - r.L2) + 0.01f, (r.L1 + r.L2) * 0.995f);
    float beta = std::atan2(-vz, -vy);
    float gam = std::acos(clampf((r.L1 * r.L1 + d * d - r.L2 * r.L2) / (2.0f * r.L1 * d), -1.0f, 1.0f));
    float knee = std::acos(clampf((r.L1 * r.L1 + r.L2 * r.L2 - d * d) / (2.0f * r.L1 * r.L2), -1.0f, 1.0f));
    float al1 = beta - gam;                     // direcao da coxa (o joelho aponta para frente)
    float al2 = al1 + (PI - knee);              // direcao da canela
    a1 = al1 - r.a0t;
    a2 = (al2 - al1) - (r.a0c - r.a0t);
    a3 = -pitch - (a1 + a2);                    // pe com inclinacao absoluta, independente da perna
}

// ---- arma na mao
// A arma fica presa ao peito (osso spine_03) e as duas maos vao ate ela por cinematica inversa. Medidas em
// "bracos": 1 = alcance do ombro ate o pulso. x + = esquerda, y + = cima, z + = frente, a partir do meio dos ombros.
static const float MUZZLE_Z = 1.27f;                                             // boca do cano, a partir da coronha
// Onde cada mao pega na arma (x, y, z a partir da coronha, no eixo do cano) e como ela fica virada:
//   HAND_AT   ponto da arma onde a palma encosta;
//   HAND_FWD  para onde apontam os nos dos dedos (do pulso para a base dos dedos);
//   HAND_PALM para onde a palma esta virada (contra a arma);
//   HAND_IN   quanto do comprimento da mao fica entre o pulso e o ponto de contato, e meia espessura da mao.
// [0] = esquerda: por baixo do guarda-mao, palma para cima, atravessada.  [1] = direita: em volta da empunhadura.
static const float HAND_AT[2][3]   = { {  0.00f, -0.058f, 0.770f }, { -0.034f, -0.100f, 0.385f } };
static const float HAND_FWD[2][3]  = { { -0.55f,  0.10f,  0.83f  }, {  0.10f,   0.35f,  0.93f  } };
static const float HAND_PALM[2][3] = { { -0.20f,  1.00f,  0.00f  }, {  1.00f,   0.00f,  0.15f  } };
static const float HAND_IN[2][2]   = { { 0.60f, 0.20f }, { 0.72f, 0.20f } };
static const float ELBOW_OUT[2] = { 0.35f, 0.75f };   // quanto cada cotovelo abre para fora (esquerdo, direito)
static const float FOREARM_TWIST = 0.5f;              // parte do giro da mao que o antebraco acompanha
static const float WEAPON_SCALE = 1.15f;     // tamanho da arma em relacao as medidas das pecas
// Tres formas de segurar, misturadas pela velocidade e por estar atirando ou nao:
//   parado / andando:    posicao de tiro -- coronha no ombro direito e o tronco um pouco de lado (ombro esquerdo a
//                        frente), com a arma girada de volta para apontar para a frente;
//   correndo:            arma atravessada na frente do corpo, cano para baixo e para a esquerda (o tronco nao muda);
//   correndo e atirando: arma reta para a frente, junto ao peito.
static const float HOLD_AIM[3] = { -0.28f, -0.10f, 0.12f }, HOLD_CARRY[3] = { -0.22f, -0.20f, 0.09f },
                   HOLD_RUNFIRE[3] = { -0.14f, -0.15f, 0.08f };
static const float HOLD_BLADE = 0.48f;       // quanto o tronco vira de lado na posicao de tiro (radianos)
static const float CARRY_YAW = 0.70f, CARRY_TILT = -0.20f;   // carregando: cano para a esquerda e para baixo

// hl = do pulso a base dos dedos; hf / hn / ht = eixos da mao em repouso (dedos, palma, polegar)
struct ArmGeom { float C[3], A, L1[2], L2[2], hl[2], hf[2][3], hn[2][3], ht[2][3]; };
static void norm3(float v[3]);
static void cross3(const float a[3], const float b[3], float o[3]) {
    o[0] = a[1] * b[2] - a[2] * b[1]; o[1] = a[2] * b[0] - a[0] * b[2]; o[2] = a[0] * b[1] - a[1] * b[0];
}

static void armGeom(const Model& m, ArmGeom& g) {
    float len[2][2];
    for (int sd = 0; sd < 2; ++sd) {
        const float* u = &m.restG[(size_t)m.upper[sd]].m[12];
        const float* l = &m.restG[(size_t)m.lower[sd]].m[12];
        const float* h = &m.restG[(size_t)m.hand[sd]].m[12];
        len[sd][0] = std::sqrt((l[0]-u[0])*(l[0]-u[0]) + (l[1]-u[1])*(l[1]-u[1]) + (l[2]-u[2])*(l[2]-u[2]));
        len[sd][1] = std::sqrt((h[0]-l[0])*(h[0]-l[0]) + (h[1]-l[1])*(h[1]-l[1]) + (h[2]-l[2])*(h[2]-l[2]));
        g.L1[sd] = len[sd][0];
        g.L2[sd] = len[sd][1];
    }
    g.A = 0.5f * (len[0][0] + len[0][1] + len[1][0] + len[1][1]);
    for (int r = 0; r < 3; ++r)
        g.C[r] = 0.5f * (m.restG[(size_t)m.upper[0]].m[12 + r] + m.restG[(size_t)m.upper[1]].m[12 + r]);
    // maos: em repouso (T-pose) os dedos apontam para fora, a palma para baixo e o polegar para a frente
    for (int sd = 0; sd < 2; ++sd) {
        const float* h = &m.restG[(size_t)m.hand[sd]].m[12];
        float f[3] = { 0, 0, 0 };
        int n = 0;
        for (size_t i = 0; i < m.fingers.size(); ++i) {
            const Model::Finger& fg = m.fingers[i];
            if (fg.side != sd || fg.thumb || fg.level != 1) continue;
            const float* q = &m.restG[(size_t)fg.joint].m[12];
            for (int c = 0; c < 3; ++c) f[c] += q[c] - h[c];
            ++n;
        }
        if (n == 0) { f[0] = sd == 0 ? 0.1f * g.A : -0.1f * g.A; n = 1; }
        for (int c = 0; c < 3; ++c) f[c] /= (float)n;
        g.hl[sd] = std::sqrt(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
        norm3(f);
        float t[3] = { -f[0] * f[2], -f[1] * f[2], 1.0f - f[2] * f[2] };      // "frente", perpendicular aos dedos
        norm3(t);
        float nn[3];
        if (sd == 0) cross3(f, t, nn); else cross3(t, f, nn);
        for (int c = 0; c < 3; ++c) { g.hf[sd][c] = f[c]; g.hn[sd][c] = nn[c]; g.ht[sd][c] = t[c]; }
    }
}

static void norm3(float v[3]) {
    float l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (l > 1e-8f) { v[0] /= l; v[1] /= l; v[2] /= l; }
}

// Gira os eixos de um osso (em torno da propria articulacao) pelo menor arco que leva a direcao a na direcao b.
static void rotateBoneArc(Mat4& g, const float a[3], const float b[3]) {
    float k[3] = { a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0] };
    float s = std::sqrt(k[0] * k[0] + k[1] * k[1] + k[2] * k[2]);
    float c = a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
    if (s < 1e-6f) return;
    k[0] /= s; k[1] /= s; k[2] /= s;
    for (int col = 0; col < 3; ++col) {
        float* v = &g.m[col * 4];
        float kv = k[0] * v[0] + k[1] * v[1] + k[2] * v[2];
        float cr[3] = { k[1] * v[2] - k[2] * v[1], k[2] * v[0] - k[0] * v[2], k[0] * v[1] - k[1] * v[0] };
        for (int r = 0; r < 3; ++r) v[r] = v[r] * c + cr[r] * s + k[r] * kv * (1.0f - c);
    }
}

// Gira os eixos de um osso em torno de uma direcao unitaria k (que passa pela articulacao).
static void rotateBoneAxis(Mat4& g, const float k[3], float ang) {
    float c = std::cos(ang), s = std::sin(ang);
    for (int col = 0; col < 3; ++col) {
        float* v = &g.m[col * 4];
        float kv = k[0] * v[0] + k[1] * v[1] + k[2] * v[2];
        float cr[3] = { k[1] * v[2] - k[2] * v[1], k[2] * v[0] - k[0] * v[2], k[0] * v[1] - k[1] * v[0] };
        for (int r = 0; r < 3; ++r) v[r] = v[r] * c + cr[r] * s + k[r] * kv * (1.0f - c);
    }
}

// Extras de pose que nao fazem parte do ciclo de passos. A caminhada e a corrida continuam exatamente como eram:
// isto so acrescenta mira para cima/baixo, coice, recarga, pulo, pouso e pequenos movimentos de quem esta parado.
struct PoseFx {
    float aimPitch = 0;     // radianos; + = mirando para baixo
    float recoil = 0;       // 1 -> 0 logo apos o tiro
    float reload = -1;      // progresso da recarga (0..1) ou -1
    float air = 0;          // 0..1 no ar
    float vy = 0;           // -1 caindo .. +1 subindo
    float land = 0;         // 1 -> 0 logo apos pousar
    float seed = 0;         // defasa os movimentos de parado entre os personagens
    float fire = 0;         // 0..1: atirando agora (correndo, a arma sai da posicao de carregar e aponta para a frente)
};

// Ponto da arma (em "bracos", relativo a origem da arma) -> relativo ao meio dos ombros, ja com o coice e a
// inclinacao da recarga. O giro e em torno da empunhadura. drawWeapon aplica a mesma transformacao.
static const float WEAPON_PIVOT[3] = { 0.0f, -0.09f, 0.38f };
static void weaponPoint(const Model& m, const float in[3], float out[3]) {
    const float K = WEAPON_SCALE;
    float x = (in[0] - WEAPON_PIVOT[0]) * K, y = (in[1] - WEAPON_PIVOT[1]) * K, z = (in[2] - WEAPON_PIVOT[2]) * K;
    float cr = std::cos(m.wRoll), sr = std::sin(m.wRoll), ct = std::cos(m.wTilt), st = std::sin(m.wTilt);
    float x1 = x * cr - y * sr, y1 = x * sr + y * cr;
    float lx = x1 + WEAPON_PIVOT[0] * K, lz = -y1 * st + z * ct + WEAPON_PIVOT[2] * K - m.wKick;
    float cy = std::cos(m.wYaw), sy = std::sin(m.wYaw);     // giro em torno da coronha
    out[0] = m.wPos[0] + lx * cy + lz * sy;
    out[1] = m.wPos[1] + y1 * ct + z * st + WEAPON_PIVOT[1] * K;
    out[2] = m.wPos[2] - lx * sy + lz * cy;
}

static float segT(float t, float a, float b) { return smooth01((t - a) / (b - a)); }

// Anima o personagem e deforma a malha.
//   cycles: fase do passo em ciclos; s: velocidade em m/s do modelo (0 = parado); time: relogio (respiracao)
//   armed: 0 = bracos soltos, 1 = segurando a arma com as duas maos
//   fx: extras (mira, coice, recarga, pulo); sem ele a pose e so o ciclo de passos
static void poseModel(Model& m, float cycles, float s, float time, float armed = 0.0f, const PoseFx* fx = NULL) {
    std::fill(m.ax.begin(), m.ax.end(), 0.0f);
    std::fill(m.ay.begin(), m.ay.end(), 0.0f);
    std::fill(m.az.begin(), m.az.end(), 0.0f);

    Gait g = gaitFor(s);
    float w = g.w, r = g.r;
    float u[2];
    u[0] = wrap01(cycles);
    u[1] = wrap01(cycles + 0.5f);

    // pernas
    float restHip = m.rig[0].hipY;
    GaitCurve gc;
    buildGaitCurve(gc, g, s, m.rig[0], restHip);
    float hipY = harmEval(gc.hip, u[0]);
    if (fx) {                                           // no ar o quadril nao oscila; ao pousar, afunda e volta
        hipY = lerpf(hipY, restHip - 0.02f, fx->air);
        hipY -= 0.085f * std::sin(PI * clampf(fx->land, 0.0f, 1.0f));
    }
    for (int sd = 0; sd < 2; ++sd) {
        const LegRig& lr = m.rig[sd];
        float dz = harmEval(gc.dz, u[sd]);
        float dy = std::max(0.0f, harmEval(gc.dy, u[sd]));
        float pitch = harmEval(gc.pitch, u[sd]);
        if (fx && fx->air > 0.0f) {                     // pulo: pernas encolhidas na subida, esticando para pousar
            float fall = clampf(-fx->vy, 0.0f, 1.0f);
            dz = lerpf(dz, sd == 0 ? 0.11f : -0.09f, fx->air);
            dy = lerpf(dy, lerpf(sd == 0 ? 0.24f : 0.16f, 0.06f, fall), fx->air);
            pitch = lerpf(pitch, -0.30f, fx->air);
        }
        float a1, a2, a3;
        solveLeg(lr, hipY, lr.ankleY + dy, lr.ankleZ + dz, pitch, a1, a2, a3);
        m.ax[(size_t)m.thigh[sd]] = a1;
        m.ax[(size_t)m.calf[sd]] = a2;
        m.ax[(size_t)m.foot[sd]] = a3;
    }

    // tronco: caminhada quase reta e calma; corrida inclinada para frente, girando e balancando mais.
    // Parado, respira.
    float lean = clampf(lerpf(0.025f + 0.01f * s, 0.10f + 0.02f * s, r), 0.0f, 0.20f) * w;
    float breath = 0.012f * std::sin(2.0f * PI * 0.25f * time) * (1.0f - w);
    float twist = lerpf(0.07f, 0.22f, r) * w * std::cos(2.0f * PI * u[0]);
    float roll = lerpf(0.025f, 0.045f, r) * w * -std::cos(2.0f * PI * (u[0] - g.beta * 0.5f));
    m.ax[(size_t)m.spine1] = lean * 0.40f;
    m.ax[(size_t)m.spine2] = lean * 0.35f;
    m.ax[(size_t)m.spine3] = lean * 0.25f + breath;
    m.ax[(size_t)m.neck] = -lean * 0.45f;
    m.ax[(size_t)m.head] = -lean * 0.40f;
    m.ay[(size_t)m.spine1] = twist * 0.25f;
    m.ay[(size_t)m.spine2] = twist * 0.40f;
    m.ay[(size_t)m.spine3] = twist * 0.35f;
    m.ay[(size_t)m.head] = -twist * 0.80f;
    m.az[(size_t)m.spine2] = roll * 0.5f;
    m.az[(size_t)m.spine3] = roll * 0.5f;
    m.az[(size_t)m.head] = -roll * 0.8f;
    // posicao de tiro: o tronco vira de lado (ombro esquerdo a frente) e a cabeca continua olhando para a frente.
    // Some conforme a corrida entra, para o corpo da corrida ficar como era.
    float blade = HOLD_BLADE * armed * (1.0f - r);
    m.ay[(size_t)m.spine1] -= blade * 0.30f;
    m.ay[(size_t)m.spine2] -= blade * 0.35f;
    m.ay[(size_t)m.spine3] -= blade * 0.35f;
    m.ay[(size_t)m.neck] += blade * 0.35f;
    m.ay[(size_t)m.head] += blade * 0.55f;
    if (fx) {
        // parado: respira mais fundo, troca o peso de lado bem devagar e olha em volta
        float idle = 1.0f - w, t = time + fx->seed;
        float sway = 0.012f * std::sin(2.0f * PI * 0.11f * t) * idle;
        m.ax[(size_t)m.spine3] += 0.010f * std::sin(2.0f * PI * 0.25f * t) * idle;
        m.az[(size_t)m.spine1] += sway;
        m.az[(size_t)m.head] -= sway * 0.6f;
        m.ay[(size_t)m.head] += (0.10f * std::sin(2.0f * PI * 0.07f * t) + 0.05f * std::sin(2.0f * PI * 0.19f * t + 1.3f)) *
                                idle * (1.0f - 0.7f * armed);
        // armado: o tronco inclina para a arma apontar para cima ou para baixo, e da um tranco no coice.
        // Some conforme a corrida entra (1 - r): correndo, o corpo fica exatamente como era.
        float ap = clampf(fx->aimPitch, -0.45f, 0.60f) * armed * (1.0f - r);
        m.ax[(size_t)m.spine1] += ap * 0.30f;
        m.ax[(size_t)m.spine2] += ap * 0.35f;
        m.ax[(size_t)m.spine3] += ap * 0.35f - 0.025f * fx->recoil * armed * (1.0f - r);
    }

    // bracos. Caminhada: pendulo solto e curto, cotovelo quase esticado. Corrida: balanco largo e rapido,
    // cotovelo em ~90 graus, que fecha mais quando a mao vem para frente.
    float armAmp = lerpf(0.08f + 0.12f * s, 0.38f + 0.045f * s, r) * w;
    float armLag = lerpf(0.06f, 0.02f, r);
    for (int sd = 0; sd < 2; ++sd) {
        float c = std::cos(2.0f * PI * (u[sd] - armLag));                   // + = braco para tras
        float fwd = 0.5f * (1.0f - std::cos(2.0f * PI * (u[sd] - armLag - lerpf(0.08f, 0.04f, r))));   // 1 = mao na frente
        float flexWalk = 0.10f + 0.25f * fwd;
        float flexRun = 1.05f + 0.40f * fwd;
        float flex = lerpf(flexWalk, flexRun, r) * w + 0.12f * (1.0f - w);
        float side = lerpf(1.36f, 1.30f, r);                                 // o modelo esta em T-pose: baixa os bracos
        m.az[(size_t)m.upper[sd]] = sd == 0 ? -side : side;
        m.ax[(size_t)m.upper[sd]] = armAmp * c + (1.0f - w) * 0.02f * std::sin(2.0f * PI * 0.2f * time + sd * 2.0f);
        m.ax[(size_t)m.lower[sd]] = -flex;
        if (fx && fx->air > 0.0f) {                     // no ar: bracos um pouco abertos e dobrados
            m.az[(size_t)m.upper[sd]] *= 1.0f - 0.18f * fx->air;
            m.ax[(size_t)m.lower[sd]] -= 0.45f * fx->air;
        }
    }

    // maos: relaxadas na caminhada, punho fechado na corrida (os dedos curvam em direcao a palma)
    float curl = lerpf(0.30f, 1.0f, r) * w + 0.22f * (1.0f - w);
    curl = lerpf(curl, 0.85f, armed);
    for (size_t i = 0; i < m.fingers.size(); ++i) {
        const Model::Finger& fg = m.fingers[i];
        float k = fg.thumb ? 0.3f : (fg.level == 1 ? 0.55f : (fg.level == 2 ? 0.85f : 0.6f));
        m.az[(size_t)fg.joint] = (fg.side == 0 ? -1.0f : 1.0f) * k * curl;
    }

    // posicao da arma nesta pose: coice e, na recarga, o cano sobe e a arma gira para a mao esquerda alcancar o carregador
    m.wKick = m.wTilt = m.wRoll = m.magDrop = 0.0f;
    m.magHidden = false;
    float fire = fx ? clampf(fx->fire, 0.0f, 1.0f) : 0.0f, carry = r * (1.0f - fire);
    for (int c = 0; c < 3; ++c) m.wPos[c] = lerpf(HOLD_AIM[c], lerpf(HOLD_CARRY[c], HOLD_RUNFIRE[c], fire), r);
    // apontando, a arma desfaz o giro e a inclinacao do tronco e fica reta para a frente; carregando, vai com o corpo
    m.wYaw = blade - twist * (1.0f - carry) + CARRY_YAW * carry;
    m.wTilt = lean * (1.0f - carry) + CARRY_TILT * carry;
    if (fx) m.wTilt -= clampf(fx->aimPitch, -0.45f, 0.60f) * r * fire;     // correndo e atirando: so a arma segue a mira
    float reloadT = -1.0f;
    if (fx && armed > 0.0f) {
        m.wKick = 0.05f * fx->recoil;
        m.wTilt += 0.03f * fx->recoil;
        if (fx->reload >= 0.0f) {
            reloadT = clampf(fx->reload, 0.0f, 1.0f);
            float env = segT(reloadT, 0.0f, 0.12f) * (1.0f - segT(reloadT, 0.86f, 1.0f));
            m.wTilt += 0.42f * env;
            m.wRoll = 0.35f * env;
            m.magDrop = reloadT < 0.5f ? 0.22f * segT(reloadT, 0.22f, 0.34f) : 0.22f * (1.0f - segT(reloadT, 0.70f, 0.84f));
            m.magHidden = reloadT > 0.36f && reloadT < 0.66f;
        }
    }

    ArmGeom ag;
    if (armed > 0.0f) armGeom(m, ag);
    // Onde o pulso tem que chegar e como a mao fica virada (no espaco do modelo), a partir da posicao da arma no peito.
    struct HandGoal { float wrist[3], f[3], n[3], t[3], turn; };
    auto handGoal = [&](int sd, HandGoal& hg) {
        float S[3], a[3], b[3], pf[3], pn[3], fc[3], nc[3], wl[3];
        for (int c = 0; c < 3; ++c) { pf[c] = HAND_AT[sd][c] + HAND_FWD[sd][c]; pn[c] = HAND_AT[sd][c] + HAND_PALM[sd][c]; }
        weaponPoint(m, HAND_AT[sd], S);
        weaponPoint(m, pf, a);
        weaponPoint(m, pn, b);
        for (int c = 0; c < 3; ++c) { fc[c] = a[c] - S[c]; nc[c] = b[c] - S[c]; }
        norm3(fc);
        float d = nc[0] * fc[0] + nc[1] * fc[1] + nc[2] * fc[2];
        for (int c = 0; c < 3; ++c) nc[c] -= fc[c] * d;
        norm3(nc);
        float hl = ag.hl[sd] / ag.A;
        for (int c = 0; c < 3; ++c) wl[c] = S[c] - (fc[c] * HAND_IN[sd][0] + nc[c] * HAND_IN[sd][1]) * hl;
        hg.turn = 1.0f;
        if (sd == 0 && reloadT >= 0.0f) {                       // recarga: guarda-mao -> carregador -> cinto -> carregador -> guarda-mao
            const float magIn[3] = { 0.03f, -0.25f - m.magDrop, 0.47f }, belt[3] = { 0.45f, -0.85f, 0.15f };
            float mg[3], k1 = segT(reloadT, 0.10f, 0.22f), k2 = segT(reloadT, 0.34f, 0.48f),
                  k3 = segT(reloadT, 0.58f, 0.70f), k4 = segT(reloadT, 0.88f, 1.0f);
            weaponPoint(m, magIn, mg);
            for (int c = 0; c < 3; ++c) {
                float v = lerpf(wl[c], mg[c], k1);
                v = lerpf(v, belt[c], k2);
                v = lerpf(v, mg[c], k3);
                wl[c] = lerpf(v, wl[c], k4);
            }
            hg.turn = 1.0f - k2 * (1.0f - k3);                  // no cinto a mao fica solta
        }
        const float* M = m.skinM[(size_t)m.spine3].m;
        float P[3];
        for (int c = 0; c < 3; ++c) P[c] = ag.C[c] + ag.A * wl[c];
        for (int c = 0; c < 3; ++c) {
            hg.wrist[c] = M[c] * P[0] + M[4 + c] * P[1] + M[8 + c] * P[2] + M[12 + c];
            hg.f[c] = M[c] * fc[0] + M[4 + c] * fc[1] + M[8 + c] * fc[2];
            hg.n[c] = M[c] * nc[0] + M[4 + c] * nc[1] + M[8 + c] * nc[2];
        }
        norm3(hg.f);
        norm3(hg.n);
        if (sd == 0) cross3(hg.n, hg.f, hg.t); else cross3(hg.f, hg.n, hg.t);
    };
    // Rotacao (eixo k, angulo) que falta para o osso ficar com os eixos de repouso levados a f / n / t.
    auto handTurn = [&](const Mat4& gm, size_t joint, int sd, const HandGoal& hg, float k[3]) -> float {
        const float* rg = m.restG[joint].m;
        float des[9], Q[9];
        for (int col = 0; col < 3; ++col) {
            const float* v = &rg[col * 4];
            float vf = v[0] * ag.hf[sd][0] + v[1] * ag.hf[sd][1] + v[2] * ag.hf[sd][2];
            float vn = v[0] * ag.hn[sd][0] + v[1] * ag.hn[sd][1] + v[2] * ag.hn[sd][2];
            float vt = v[0] * ag.ht[sd][0] + v[1] * ag.ht[sd][1] + v[2] * ag.ht[sd][2];
            for (int r2 = 0; r2 < 3; ++r2) des[col * 3 + r2] = hg.f[r2] * vf + hg.n[r2] * vn + hg.t[r2] * vt;
        }
        for (int r2 = 0; r2 < 3; ++r2)                           // Q = desejado * atual^T
            for (int c2 = 0; c2 < 3; ++c2)
                Q[c2 * 3 + r2] = des[r2] * gm.m[c2] + des[3 + r2] * gm.m[4 + c2] + des[6 + r2] * gm.m[8 + c2];
        float ang = std::acos(clampf((Q[0] + Q[4] + Q[8] - 1.0f) * 0.5f, -1.0f, 1.0f));
        k[0] = Q[1 * 3 + 2] - Q[2 * 3 + 1];
        k[1] = Q[2 * 3 + 0] - Q[0 * 3 + 2];
        k[2] = Q[0 * 3 + 1] - Q[1 * 3 + 0];
        float l = std::sqrt(k[0] * k[0] + k[1] * k[1] + k[2] * k[2]);
        if (l < 1e-5f) return 0.0f;
        k[0] /= l; k[1] /= l; k[2] /= l;
        return ang;
    };
    std::vector<char> isFinger(m.joints.size(), 0);
    for (size_t i = 0; i < m.fingers.size(); ++i) isFinger[(size_t)m.fingers[i].joint] = 1;

    std::vector<Mat4>& G = m.G;
    for (size_t i = 0; i < m.joints.size(); ++i) {
        const Joint& j = m.joints[i];
        Mat4 gm = j.parent < 0 ? j.rest : mul(G[(size_t)j.parent], j.rest);
        if (isFinger[i] && armed > 0.0f && m.az[i] != 0.0f) {
            // os dedos fecham em torno do eixo Z da propria mao (que gira junto com o braco)
            const float* pm = m.skinM[(size_t)j.parent].m;
            float k[3] = { pm[8] * armed, pm[9] * armed, pm[10] * armed };
            k[2] += 1.0f - armed;
            norm3(k);
            rotateBoneAxis(gm, k, m.az[i]);
        } else if (m.az[i] != 0.0f) rotateBoneZ(gm, m.az[i]);
        if (m.ay[i] != 0.0f) rotateBoneY(gm, m.ay[i]);
        if (m.ax[i] != 0.0f) rotateBoneX(gm, m.ax[i]);
        if ((int)i == m.root) gm.m[13] += hipY - restHip;

        // segurando a arma: o braco aponta para onde o pulso precisa chegar (misturado com a pose normal)
        for (int sd = 0; armed > 0.0f && sd < 2; ++sd) {
            bool isUpper = (int)i == m.upper[sd], isLower = (int)i == m.lower[sd];
            if (!isUpper && !isLower) continue;
            const float* o = &m.joints[(size_t)(isUpper ? m.lower[sd] : m.hand[sd])].rest.m[12];
            float cur[3];
            for (int c = 0; c < 3; ++c) cur[c] = gm.m[c] * o[0] + gm.m[4 + c] * o[1] + gm.m[8 + c] * o[2];
            norm3(cur);

            HandGoal hg;
            handGoal(sd, hg);
            const float* T = hg.wrist;

            float S[3] = { gm.m[12], gm.m[13], gm.m[14] };
            float tgt[3] = { T[0] - S[0], T[1] - S[1], T[2] - S[2] };
            if (isUpper) {                                      // 2 ossos: acha o cotovelo (para fora e para baixo)
                float d = std::sqrt(tgt[0] * tgt[0] + tgt[1] * tgt[1] + tgt[2] * tgt[2]);
                float L1 = ag.L1[sd], L2 = ag.L2[sd];
                d = clampf(d, std::fabs(L1 - L2) + 0.01f, (L1 + L2) * 0.999f);
                float un[3] = { tgt[0], tgt[1], tgt[2] };
                norm3(un);
                float a = (L1 * L1 - L2 * L2 + d * d) / (2.0f * d);
                float h = std::sqrt(std::max(L1 * L1 - a * a, 0.0f));
                float pole[3] = { sd == 0 ? ELBOW_OUT[0] : -ELBOW_OUT[1], -1.0f, -0.1f };
                float pu = pole[0] * un[0] + pole[1] * un[1] + pole[2] * un[2];
                float perp[3] = { pole[0] - un[0] * pu, pole[1] - un[1] * pu, pole[2] - un[2] * pu };
                norm3(perp);
                for (int c = 0; c < 3; ++c) tgt[c] = un[c] * a + perp[c] * h;
            }
            norm3(tgt);
            float mix[3];
            for (int c = 0; c < 3; ++c) mix[c] = cur[c] * (1.0f - armed) + tgt[c] * armed;
            norm3(mix);
            rotateBoneArc(gm, cur, mix);
            if (isLower && hg.turn > 0.0f) {                    // o antebraco acompanha parte do giro da mao (em torno de si)
                Mat4 hm = mul(gm, m.joints[(size_t)m.hand[sd]].rest);
                float k[3], ang = handTurn(hm, (size_t)m.hand[sd], sd, hg, k);
                float ka = k[0] * mix[0] + k[1] * mix[1] + k[2] * mix[2];
                float tw = 2.0f * std::atan2(std::sin(ang * 0.5f) * ka, std::cos(ang * 0.5f));
                if (tw > PI) tw -= 2.0f * PI;
                if (tw < -PI) tw += 2.0f * PI;
                rotateBoneAxis(gm, mix, tw * FOREARM_TWIST * armed * hg.turn);
            }
        }
        // a mao vira para envolver a arma
        for (int sd = 0; armed > 0.0f && sd < 2; ++sd) {
            if ((int)i != m.hand[sd]) continue;
            HandGoal hg;
            handGoal(sd, hg);
            float k[3], ang = handTurn(gm, i, sd, hg, k);
            if (ang != 0.0f && hg.turn > 0.0f) rotateBoneAxis(gm, k, ang * armed * hg.turn);
        }
        G[i] = gm;
        m.skinM[i] = mul(gm, j.inv);
    }

    // skinning na CPU
    for (size_t v = 0; v < m.verts.size(); ++v) {
        const Vertex& vt = m.verts[v];
        float a[4][3] = {};
        for (int k = 0; k < 4; ++k) {
            float w = vt.w[k];
            if (w <= 0.0f) continue;
            const float* sm = m.skinM[vt.j[k]].m;
            for (int col = 0; col < 4; ++col)
                for (int r = 0; r < 3; ++r) a[col][r] += w * sm[col * 4 + r];
        }
        for (int r = 0; r < 3; ++r) {
            m.skinP[v * 3 + r] = a[0][r] * vt.p[0] + a[1][r] * vt.p[1] + a[2][r] * vt.p[2] + a[3][r];
            m.skinN[v * 3 + r] = a[0][r] * vt.n[0] + a[1][r] * vt.n[1] + a[2][r] * vt.n[2];
        }
    }
}

static GLuint textureId(int id);   // texturas geradas por codigo (definidas mais abaixo)

// hurt (0..1): clarao vermelho quando o personagem leva um tiro
static void drawModel(Model& m, float tint, const Look& look, float hurt = 0.0f) {
    auto col = [hurt](float r, float g, float b) {
        glColor3f(r + 0.28f * hurt, g * (1.0f - 0.55f * hurt), b * (1.0f - 0.55f * hurt));
    };
    glPushMatrix();
    glScalef(m.scale, m.scale, m.scale);
    glTranslatef(0.0f, -m.minY, 0.0f);
    glEnable(GL_TEXTURE_2D);
    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_NORMAL_ARRAY);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    glVertexPointer(3, GL_FLOAT, 0, m.skinP.data());
    glNormalPointer(GL_FLOAT, 0, m.skinN.data());
    glTexCoordPointer(2, GL_FLOAT, sizeof(Vertex), m.verts[0].uv);
    for (size_t i = 0; i < m.parts.size(); ++i) {
        const Part& pt = m.parts[i];
        if (pt.type == 2 && pt.id != look.hair) continue;
        if (pt.type == 3 && !look.beard) continue;
        if (pt.type == 4) {                                 // roupa: cor da peca vezes a trama do tecido (repetida)
            if (!look.outfit) continue;
            const float* c = CLOTH_COLOR[pt.id];
            static const float enemyVest[3] = { 0.50f, 0.10f, 0.10f }, blueVest[3] = { 0.10f, 0.20f, 0.52f };
            if (look.team == 1 && pt.id == CL_VEST) c = enemyVest;       // o bot (e o inimigo na rede) usa colete vermelho
            if (look.team == 2 && pt.id == CL_VEST) c = blueVest;        // equipe azul
            bool web = pt.id == CL_VEST || pt.id == CL_BELT;             // colete e cinto: fitas costuradas
            float rep = web ? 7.0f : (pt.id == CL_BOOTS || pt.id == CL_GLOVES ? 10.0f : 14.0f);
            glBindTexture(GL_TEXTURE_2D, textureId(web ? 1 : 0));
            glMatrixMode(GL_TEXTURE);
            glLoadIdentity();
            glScalef(rep, rep, 1.0f);
            col(tint * c[0] * 1.25f, tint * c[1] * 1.25f, tint * c[2] * 1.25f);
            glDrawElements(GL_TRIANGLES, (GLsizei)pt.count, GL_UNSIGNED_INT, &m.idx[pt.first]);
            glLoadIdentity();
            glMatrixMode(GL_MODELVIEW);
            continue;
        }
        // cabelo, barba e sobrancelhas usam uma textura cinza que recebe a cor escolhida
        float k = pt.type == 1 ? 0.8f : 1.0f;
        if (pt.type == 0) col(tint, tint, tint);
        else col(tint * k * COLOR_RGB[look.color][0], tint * k * COLOR_RGB[look.color][1],
                 tint * k * COLOR_RGB[look.color][2]);
        glBindTexture(GL_TEXTURE_2D, m.tex[(size_t)pt.tex]);
        glDrawElements(GL_TRIANGLES, (GLsizei)pt.count, GL_UNSIGNED_INT, &m.idx[pt.first]);
    }
    glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    glDisableClientState(GL_NORMAL_ARRAY);
    glDisableClientState(GL_VERTEX_ARRAY);
    glDisable(GL_TEXTURE_2D);
    glPopMatrix();
}

static Model g_models[2];   // 0 = homem, 1 = mulher

// ---------------------------------------------------------------- desenho

// Cubo unitario centrado na origem, esticado para (sx, sy, sz).
static void drawBox(float sx, float sy, float sz) {
    glPushMatrix();
    glScalef(sx, sy, sz);
    glBegin(GL_QUADS);
    glNormal3f(0, 0, 1);  glVertex3f(-.5f, -.5f, .5f);  glVertex3f(.5f, -.5f, .5f);   glVertex3f(.5f, .5f, .5f);    glVertex3f(-.5f, .5f, .5f);
    glNormal3f(0, 0, -1); glVertex3f(.5f, -.5f, -.5f);  glVertex3f(-.5f, -.5f, -.5f); glVertex3f(-.5f, .5f, -.5f);  glVertex3f(.5f, .5f, -.5f);
    glNormal3f(1, 0, 0);  glVertex3f(.5f, -.5f, .5f);   glVertex3f(.5f, -.5f, -.5f);  glVertex3f(.5f, .5f, -.5f);   glVertex3f(.5f, .5f, .5f);
    glNormal3f(-1, 0, 0); glVertex3f(-.5f, -.5f, -.5f); glVertex3f(-.5f, -.5f, .5f);  glVertex3f(-.5f, .5f, .5f);   glVertex3f(-.5f, .5f, -.5f);
    glNormal3f(0, 1, 0);  glVertex3f(-.5f, .5f, .5f);   glVertex3f(.5f, .5f, .5f);    glVertex3f(.5f, .5f, -.5f);   glVertex3f(-.5f, .5f, -.5f);
    glNormal3f(0, -1, 0); glVertex3f(-.5f, -.5f, -.5f); glVertex3f(.5f, -.5f, -.5f);  glVertex3f(.5f, -.5f, .5f);   glVertex3f(-.5f, -.5f, .5f);
    glEnd();
    glPopMatrix();
}

// Membro que balanca a partir da articulacao (x, y, z).
static void drawLimb(float x, float y, float z, float w, float h, float d, float angle,
                     float r, float g, float b) {
    glPushMatrix();
    glTranslatef(x, y, z);
    glRotatef(angle * 180.0f / PI, 1, 0, 0);
    glTranslatef(0, -h * 0.5f, 0);
    glColor3f(r, g, b);
    drawBox(w, h, d);
    glPopMatrix();
}

static void drawGround(float cx, float cz) {
    const float T = 2.0f;
    const int R = 50;
    int ix0 = (int)std::floor(cx / T);
    int iz0 = (int)std::floor(cz / T);

    glNormal3f(0, 1, 0);
    glBegin(GL_QUADS);
    for (int iz = iz0 - R; iz <= iz0 + R; ++iz) {
        for (int ix = ix0 - R; ix <= ix0 + R; ++ix) {
            if ((ix + iz) & 1) glColor3f(0.33f, 0.62f, 0.31f);
            else               glColor3f(0.27f, 0.54f, 0.26f);
            float x = ix * T, z = iz * T;
            glVertex3f(x, 0, z + T);
            glVertex3f(x + T, 0, z + T);
            glVertex3f(x + T, 0, z);
            glVertex3f(x, 0, z);
        }
    }
    glEnd();
}

// ---------------------------------------------------------------- texturas geradas por codigo
// Nada de arquivos de imagem: concreto, madeira, metal, tecido e brilhos sao calculados ao abrir o jogo, com ruido
// que se repete sem emenda nas bordas.
#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif
#define GLX_TEXTURE_MAX_ANISOTROPY 0x84FE
#define GLX_MAX_TEXTURE_MAX_ANISOTROPY 0x84FF
#define GLX_MULTISAMPLE 0x809D

enum TexId { TX_FLOOR, TX_WALL, TX_CONCRETE, TX_CRATE, TX_CONTAINER, TX_BARRIER, TX_HAZARD, TX_CEIL, TX_PAINT,
             TX_SAND, TX_ROCK, TX_ADOBE, TX_SANDBAG, TX_WHITE, TX_GLOW, TX_FLASH, TX_FABRIC, TX_WEB, NUM_TX };
static GLuint g_tex[NUM_TX];
static float g_maxAniso = 0.0f;         // filtro anisotropico (chao nitido em angulo raso), se a placa tiver

static unsigned hash2(int x, int y, unsigned seed) {
    unsigned h = (unsigned)x * 374761393u + (unsigned)y * 668265263u + seed * 2246822519u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return h ^ (h >> 16);
}
static float hashf(int x, int y, unsigned seed) { return (float)(hash2(x, y, seed) & 0xFFFFFF) / 16777216.0f; }

// Ruido suave de periodo (px, py): o valor em x + px e igual ao valor em x.
static float vnoise(float x, float y, int px, int py, unsigned seed) {
    int x0 = (int)std::floor(x), y0 = (int)std::floor(y);
    float fx = x - x0, fy = y - y0;
    fx = fx * fx * (3.0f - 2.0f * fx);
    fy = fy * fy * (3.0f - 2.0f * fy);
    int xa = ((x0 % px) + px) % px, xb = (xa + 1) % px, ya = ((y0 % py) + py) % py, yb = (ya + 1) % py;
    float a = hashf(xa, ya, seed), b = hashf(xb, ya, seed), c = hashf(xa, yb, seed), d = hashf(xb, yb, seed);
    return (a + (b - a) * fx) * (1.0f - fy) + (c + (d - c) * fx) * fy;
}

// Soma de oitavas de ruido (0..1). u, v em 0..1; fu, fv = quantas "manchas" cabem em cada direcao na primeira oitava.
static float fbm2(float u, float v, int fu, int fv, int oct, unsigned seed) {
    float sum = 0.0f, amp = 0.5f, norm = 0.0f;
    for (int o = 0; o < oct; ++o) {
        sum += amp * vnoise(u * fu, v * fv, fu, fv, seed + (unsigned)o * 17u);
        norm += amp;
        amp *= 0.5f; fu *= 2; fv *= 2;
    }
    return sum / norm;
}
static float fbm(float u, float v, int f, int oct, unsigned seed) { return fbm2(u, v, f, f, oct, seed); }
static float fracf(float x) { return x - std::floor(x); }

struct Img {
    int w, h;
    std::vector<unsigned char> px;
    Img(int w_, int h_) : w(w_), h(h_), px((size_t)w_ * h_ * 4, 255) {}
    void set(int x, int y, float r, float g, float b, float a = 1.0f) {
        unsigned char* p = &px[((size_t)y * w + x) * 4];
        p[0] = (unsigned char)(clampf(r, 0.0f, 1.0f) * 255.0f + 0.5f);
        p[1] = (unsigned char)(clampf(g, 0.0f, 1.0f) * 255.0f + 0.5f);
        p[2] = (unsigned char)(clampf(b, 0.0f, 1.0f) * 255.0f + 0.5f);
        p[3] = (unsigned char)(clampf(a, 0.0f, 1.0f) * 255.0f + 0.5f);
    }
};

static GLuint uploadTexture(const Img& im, bool repeat) {
    GLuint t;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, repeat ? GL_REPEAT : GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, repeat ? GL_REPEAT : GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_GENERATE_MIPMAP, GL_TRUE);
    if (g_maxAniso > 1.0f) glTexParameterf(GL_TEXTURE_2D, GLX_TEXTURE_MAX_ANISOTROPY, std::min(g_maxAniso, 8.0f));
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, im.w, im.h, 0, GL_RGBA, GL_UNSIGNED_BYTE, im.px.data());
    return t;
}

// Piso de concreto polido: 4 x 4 placas com juntas, manchas, poros e algumas trincas. Cobre 10 x 10 unidades.
static void genFloor(Img& im) {
    const int N = im.w;
    std::vector<float> crack((size_t)N * N, 0.0f);
    unsigned rs = 777u;
    auto rnd = [&rs]() { rs = rs * 1664525u + 1013904223u; return (float)((rs >> 8) & 0xFFFF) / 65536.0f; };
    for (int k = 0; k < 10; ++k) {
        float x = rnd() * N, y = rnd() * N, a = rnd() * 6.283f;
        int steps = 120 + (int)(rnd() * 260.0f);
        for (int i = 0; i < steps; ++i) {
            a += (rnd() - 0.5f) * 0.7f;
            x += std::cos(a); y += std::sin(a);
            int xi = (((int)x % N) + N) % N, yi = (((int)y % N) + N) % N;
            crack[(size_t)yi * N + xi] = 1.0f;
            float& nb = crack[(size_t)yi * N + (xi + 1) % N];
            nb = std::max(nb, 0.4f);
        }
    }
    for (int y = 0; y < N; ++y)
        for (int x = 0; x < N; ++x) {
            float u = (x + 0.5f) / N, v = (y + 0.5f) / N;
            float c = 0.56f + 0.16f * (fbm(u, v, 3, 5, 11u) - 0.5f) + 0.07f * (fbm(u, v, 48, 3, 23u) - 0.5f);
            float st = fbm(u, v, 5, 4, 37u);
            if (st > 0.58f) c -= 0.10f * smooth01((st - 0.58f) / 0.2f);            // manchas escuras
            int sx = (int)(u * 4.0f), sy = (int)(v * 4.0f);
            c += (hashf(sx, sy, 5u) - 0.5f) * 0.05f;                              // cada placa com um tom
            float du = u * 4.0f - sx, dv = v * 4.0f - sy;
            float dj = std::min(std::min(du, 1.0f - du), std::min(dv, 1.0f - dv)) * N / 4.0f;   // pixels ate a junta
            if (dj < 1.6f) c *= 0.55f; else if (dj < 3.5f) c *= 0.88f;
            c *= 1.0f - 0.55f * crack[(size_t)y * N + x];
            float sp = hashf(x, y, 91u);
            if (sp > 0.992f) c *= 0.78f; else if (sp < 0.006f) c *= 1.12f;         // poros e graos
            im.set(x, y, c * 0.97f, c * 0.99f, c * 1.03f);
        }
}

// Parede de concreto moldado: paineis de 4 x 4 com emendas, furos dos tirantes e escorridos. Cobre 8 x 8 unidades.
static void genWall(Img& im) {
    const int N = im.w;
    for (int y = 0; y < N; ++y)
        for (int x = 0; x < N; ++x) {
            float u = (x + 0.5f) / N, v = (y + 0.5f) / N;
            float c = 0.60f + 0.10f * (fbm(u, v, 4, 5, 101u) - 0.5f) + 0.05f * (fbm(u, v, 64, 2, 103u) - 0.5f);
            float streak = fbm2(u, v, 24, 2, 3, 107u);                            // escorridos verticais
            c -= 0.08f * smooth01((streak - 0.55f) / 0.25f);
            float du = fracf(u * 2.0f), dv = fracf(v * 2.0f);
            float ds = std::min(std::min(du, 1.0f - du), std::min(dv, 1.0f - dv)) * N / 2.0f;
            if (ds < 1.5f) c *= 0.60f; else if (ds < 3.0f) c *= 0.90f;
            float hx = fracf(u * 8.0f) - 0.5f, hy = fracf(v * 8.0f) - 0.5f;
            float hr = std::sqrt(hx * hx + hy * hy) * N / 8.0f;
            if (hr < 2.2f) c *= 0.55f; else if (hr < 3.5f) c *= 1.06f;
            im.set(x, y, c * 0.98f, c, c * 1.03f);
        }
}

// Concreto mais aspero, com as linhas das tabuas da forma. Muros, pilares e topo das barreiras. Cobre 4 x 4 unidades.
static void genConcrete(Img& im) {
    const int N = im.w;
    for (int y = 0; y < N; ++y)
        for (int x = 0; x < N; ++x) {
            float u = (x + 0.5f) / N, v = (y + 0.5f) / N;
            float c = 0.64f + 0.14f * (fbm(u, v, 4, 5, 201u) - 0.5f) + 0.08f * (fbm(u, v, 40, 3, 203u) - 0.5f);
            float dv = fracf(v * 8.0f);
            if (std::min(dv, 1.0f - dv) * N / 8.0f < 1.0f) c *= 0.80f;
            if (hashf(x, y, 207u) > 0.986f) c *= 0.70f;
            im.set(x, y, c * 1.02f, c, c * 0.96f);
        }
}

// Uma face de caixote: tabuas verticais, moldura e travessa diagonal.
static void genCrate(Img& im) {
    const int N = im.w;
    const float fw = 0.11f;
    for (int y = 0; y < N; ++y)
        for (int x = 0; x < N; ++x) {
            float u = (x + 0.5f) / N, v = (y + 0.5f) / N;
            float c;
            float edge = std::min(std::min(u, 1.0f - u), std::min(v, 1.0f - v));
            if (edge < fw) {                                                     // moldura
                bool horiz = std::min(v, 1.0f - v) < std::min(u, 1.0f - u);
                float grain = horiz ? fbm2(u, v, 3, 40, 3, 317u) : fbm2(u, v, 40, 3, 3, 319u);
                c = 1.00f * (0.85f + 0.30f * (grain - 0.5f));
                if (fw - edge < 0.012f) c *= 0.55f;                              // sombra da borda de dentro
                float nx = std::min(u, 1.0f - u) - fw * 0.5f, ny = std::min(v, 1.0f - v) - fw * 0.5f;
                if (nx * nx + ny * ny < 0.00022f) c *= 0.35f;                    // pregos nos cantos
            } else {
                int pid = (int)(u * 5.0f);
                float tone = 0.80f + 0.20f * hashf(pid, 0, 301u);
                c = tone * (0.85f + 0.30f * (fbm2(u, v, 40, 3, 3, 303u + (unsigned)pid) - 0.5f));
                float gp = fracf(u * 5.0f);
                if (std::min(gp, 1.0f - gp) * N / 5.0f < 1.3f) c *= 0.45f;       // fresta entre tabuas
                float d = std::fabs(u - v) * 0.7071f;                            // travessa diagonal
                if (d < 0.065f) {
                    c = 0.95f * (0.85f + 0.30f * (fbm2(u + v, u - v + 1.0f, 3, 40, 3, 311u) - 0.5f));
                    if (d > 0.055f) c *= 0.55f;
                }
            }
            if (edge < 0.008f) c *= 0.6f;
            im.set(x, y, 0.74f * c, 0.54f * c, 0.31f * c);
        }
}

// Chapa ondulada de container (tons de cinza: a cor vem do proprio container), com trilhos, riscos e sujeira embaixo.
static void genContainer(Img& im) {
    const int N = im.w;
    for (int y = 0; y < N; ++y)
        for (int x = 0; x < N; ++x) {
            float u = (x + 0.5f) / N, v = (y + 0.5f) / N;
            float t = fracf(u * 10.0f);
            float s = t < 0.15f ? t / 0.15f : (t < 0.5f ? 1.0f : (t < 0.65f ? 1.0f - (t - 0.5f) / 0.15f : 0.0f));
            float c = (0.68f + 0.32f * s) * (0.90f + 0.20f * (fbm(u, v, 6, 4, 401u) - 0.5f));
            if (v < 0.05f || v > 0.95f) c = 0.60f * (0.9f + 0.2f * (fbm(u, v, 16, 2, 409u) - 0.5f));    // trilhos
            if (fbm2(u, v, 3, 96, 2, 403u) > 0.70f) c *= 1.12f;                                         // riscos
            float d = smooth01((0.32f - v) / 0.32f) * smooth01((fbm(u, v, 10, 4, 407u) - 0.35f) / 0.3f);
            im.set(x, y, c * (1.0f - 0.30f * d), c * (1.0f - 0.45f * d), c * (1.0f - 0.55f * d));
        }
}

static void hazardColor(float s, float wear, float c[3]) {     // faixa amarela e preta, gasta
    if (s < 0.5f) { c[0] = 0.93f * wear; c[1] = 0.72f * wear; c[2] = 0.10f * wear; }
    else          { c[0] = 0.10f * wear; c[1] = 0.10f * wear; c[2] = 0.11f * wear; }
}

// Barreira de concreto com faixa de alerta no alto.
static void genBarrier(Img& im) {
    const int N = im.w;
    for (int y = 0; y < N; ++y)
        for (int x = 0; x < N; ++x) {
            float u = (x + 0.5f) / N, v = (y + 0.5f) / N;
            float c = 0.66f + 0.14f * (fbm(u, v, 4, 5, 501u) - 0.5f) + 0.08f * (fbm(u, v, 40, 3, 503u) - 0.5f);
            float col[3] = { c * 1.02f, c, c * 0.96f };
            if (v > 0.60f && fbm(u, v, 12, 4, 507u) < 0.68f) {                   // tinta lascada em alguns pontos
                hazardColor(fracf(u * 4.0f + v * 1.5f), 0.80f + 0.35f * (fbm(u, v, 24, 3, 509u) - 0.3f), col);
                if (v < 0.615f) { col[0] *= 0.5f; col[1] *= 0.5f; col[2] *= 0.5f; }
            }
            im.set(x, y, col[0], col[1], col[2]);
        }
}

static void genHazard(Img& im) {
    const int N = im.w;
    for (int y = 0; y < N; ++y)
        for (int x = 0; x < N; ++x) {
            float u = (x + 0.5f) / N, v = (y + 0.5f) / N;
            float col[3];
            hazardColor(fracf((u + v) * 4.0f), 0.78f + 0.35f * (fbm(u, v, 16, 3, 521u) - 0.3f), col);
            if (fbm(u, v, 10, 4, 523u) > 0.70f) { float c = 0.6f; col[0] = c; col[1] = c * 0.98f; col[2] = c * 0.94f; }
            im.set(x, y, col[0], col[1], col[2]);
        }
}

// Telhado metalico escuro, com nervuras.
static void genCeil(Img& im) {
    const int N = im.w;
    for (int y = 0; y < N; ++y)
        for (int x = 0; x < N; ++x) {
            float u = (x + 0.5f) / N, v = (y + 0.5f) / N;
            float c = 0.34f * (0.72f + 0.28f * std::fabs(std::sin(PI * u * 12.0f))) * (0.88f + 0.24f * (fbm(u, v, 8, 3, 541u) - 0.5f));
            im.set(x, y, c * 0.94f, c, c * 1.10f);
        }
}

// Tinta amarela de demarcacao no piso, gasta (transparente onde descascou).
static void genPaint(Img& im) {
    const int N = im.w;
    for (int y = 0; y < N; ++y)
        for (int x = 0; x < N; ++x) {
            float u = (x + 0.5f) / N, v = (y + 0.5f) / N;
            float k = 0.85f + 0.30f * (fbm(u, v, 16, 3, 601u) - 0.5f);
            float a = smooth01((fbm(u, v, 8, 4, 603u) - 0.30f) / 0.15f) * 0.90f;
            im.set(x, y, 0.95f * k, 0.74f * k, 0.12f * k, a);
        }
}

// Mancha redonda suave (brilhos, sombras, fumaca, faiscas).
static void genGlow(Img& im) {
    const int N = im.w;
    for (int y = 0; y < N; ++y)
        for (int x = 0; x < N; ++x) {
            float dx = (x + 0.5f) / N * 2.0f - 1.0f, dy = (y + 0.5f) / N * 2.0f - 1.0f;
            float r = std::sqrt(dx * dx + dy * dy), k = std::max(0.0f, 1.0f - r);
            im.set(x, y, 1.0f, 1.0f, 1.0f, k * k * (3.0f - 2.0f * k));
        }
}

// Clarao do tiro: nucleo claro com seis pontas.
static void genFlash(Img& im) {
    const int N = im.w;
    for (int y = 0; y < N; ++y)
        for (int x = 0; x < N; ++x) {
            float dx = (x + 0.5f) / N * 2.0f - 1.0f, dy = (y + 0.5f) / N * 2.0f - 1.0f;
            float r = std::sqrt(dx * dx + dy * dy), th = std::atan2(dy, dx);
            float spike = std::pow(std::fabs(std::cos(3.0f * th)), 8.0f);
            float a = std::max(std::exp(-r * 6.0f) * 1.3f, spike * std::exp(-r * 3.0f) * std::max(0.0f, 1.0f - r));
            im.set(x, y, 1.0f, 0.92f, 0.70f, a);
        }
}

// Tecido tatico: trama fina, quadriculado de ripstop e leves manchas. Tons de cinza (a cor vem da peca de roupa).
static void genFabric(Img& im) {
    const int N = im.w;
    for (int y = 0; y < N; ++y)
        for (int x = 0; x < N; ++x) {
            float u = (x + 0.5f) / N, v = (y + 0.5f) / N;
            float c = 0.84f + 0.07f * std::sin(2.0f * PI * u * 32.0f) * std::sin(2.0f * PI * v * 32.0f) + 0.05f * (hashf(x, y, 701u) - 0.5f);
            float gu = fracf(u * 8.0f), gv = fracf(v * 8.0f);
            if (std::min(gu, 1.0f - gu) * N / 8.0f < 0.8f || std::min(gv, 1.0f - gv) * N / 8.0f < 0.8f) c *= 0.88f;
            c *= 0.82f + 0.30f * fbm(u, v, 3, 3, 703u);
            im.set(x, y, c, c, c);
        }
}

// Colete: fileiras de fitas costuradas (MOLLE).
static void genWeb(Img& im) {
    const int N = im.w;
    for (int y = 0; y < N; ++y)
        for (int x = 0; x < N; ++x) {
            float u = (x + 0.5f) / N, v = (y + 0.5f) / N;
            float t = fracf(v * 6.0f), c;
            if (t < 0.55f) {
                c = 0.98f;
                if (t < 0.06f || t > 0.49f) c = 0.72f;
                if (fracf(u * 8.0f) < 0.09f) c = 0.66f;                          // costuras
            } else c = 0.68f;
            c *= 0.92f + 0.10f * std::sin(2.0f * PI * u * 40.0f) * std::sin(2.0f * PI * v * 40.0f) + 0.06f * (hashf(x, y, 711u) - 0.5f);
            im.set(x, y, c, c, c);
        }
}

// ---- texturas dos outros mapas
// Chao do deserto: areia batida com ondas de vento, pedrinhas e trechos de terra seca rachada. Cobre 10 x 10 unidades.
static void genSand(Img& im) {
    const int N = im.w, C = 14;                             // C x C placas de terra rachada
    for (int y = 0; y < N; ++y)
        for (int x = 0; x < N; ++x) {
            float u = (x + 0.5f) / N, v = (y + 0.5f) / N;
            float c = 0.80f + 0.20f * (fbm(u, v, 3, 4, 301u) - 0.5f) + 0.10f * (fbm(u, v, 64, 2, 303u) - 0.5f);
            float loose = smooth01((fbm(u, v, 2, 3, 311u) - 0.42f) / 0.2f);          // 1 = areia solta, 0 = terra dura
            float wave = std::sin((v * 46.0f + 2.2f * fbm(u, v, 4, 3, 307u)) * 2.0f * PI);
            c += 0.035f * wave * loose;                                              // ondas de vento na areia solta
            float gx = u * C, gy = v * C, d1 = 9.0f, d2 = 9.0f;
            int cx = (int)std::floor(gx), cy = (int)std::floor(gy);
            for (int j = -1; j <= 1; ++j)
                for (int i = -1; i <= 1; ++i) {
                    int ax = cx + i, ay = cy + j, wx = ((ax % C) + C) % C, wy = ((ay % C) + C) % C;
                    float px = ax + hashf(wx, wy, 313u), py = ay + hashf(wx, wy, 317u);
                    float d = std::sqrt((gx - px) * (gx - px) + (gy - py) * (gy - py));
                    if (d < d1) { d2 = d1; d1 = d; } else if (d < d2) d2 = d;
                }
            float crack = (1.0f - smooth01((d2 - d1) / 0.07f)) * (1.0f - loose);     // d2 - d1 perto de 0: em cima da rachadura
            c *= 1.0f - 0.38f * crack;
            float sp = hashf(x, y, 331u);
            if (sp > 0.994f) c *= 0.72f; else if (sp < 0.008f) c *= 1.10f;           // pedrinhas e graos claros
            float red = 0.05f * (fbm(u, v, 6, 3, 337u) - 0.5f);
            im.set(x, y, c * (1.00f + red), c * 0.84f, c * (0.62f - red));
        }
}

// Arenito: camadas horizontais de tons diferentes, levemente onduladas, com fendas verticais. Cobre 8 x 8 unidades.
static void genRock(Img& im) {
    const int N = im.w, L = 22;
    for (int y = 0; y < N; ++y)
        for (int x = 0; x < N; ++x) {
            float u = (x + 0.5f) / N, v = (y + 0.5f) / N;
            float vv = v + 0.05f * (fbm(u, v, 3, 3, 401u) - 0.5f);
            float lf = vv * L + (float)L;
            float band = hashf((int)std::floor(lf) % L, 0, 405u);
            float layer = fbm2(u, vv, 2, 14, 3, 403u);
            float c = 0.64f + 0.20f * (layer - 0.5f) + 0.16f * (band - 0.5f) + 0.10f * (fbm(u, v, 48, 2, 407u) - 0.5f);
            if (fracf(lf) < 0.08f) c *= 0.80f;                                       // sombra entre as camadas
            float cr = fbm2(u, v, 18, 3, 3, 409u);
            c -= 0.15f * smooth01((cr - 0.62f) / 0.12f);                             // fendas
            im.set(x, y, c * 1.10f, c * (0.72f + 0.10f * (band - 0.5f)), c * (0.50f + 0.10f * (layer - 0.5f)));
        }
}

// Fiadas desencontradas: tijolos de adobe com o reboco de barro caindo, ou sacos de areia empilhados.
static void genCourses(Img& im, int cols, int rows, bool bags, unsigned seed) {
    const int N = im.w;
    for (int y = 0; y < N; ++y)
        for (int x = 0; x < N; ++x) {
            float u = (x + 0.5f) / N, v = (y + 0.5f) / N;
            float fy = v * rows;
            int ry = (int)fy;
            float fx = u * cols + ((ry & 1) ? 0.5f : 0.0f);
            int rx = (int)std::floor(fx) % cols;
            float du = fracf(fx), dv = fy - ry, tone = hashf(rx, ry, seed), c;
            if (bags) {                                                             // cheio no meio, afinando nas bordas
                float ex = std::min(du, 1.0f - du) * 2.0f, ey = std::min(dv, 1.0f - dv) * 2.0f;
                float round = std::sqrt(clampf(ex * 3.0f, 0.0f, 1.0f)) * std::sqrt(clampf(ey * 2.2f, 0.0f, 1.0f));
                c = (0.66f + 0.16f * (tone - 0.5f)) * (0.42f + 0.58f * round);
                c *= 0.93f + 0.07f * (float)(((x / 2) + (y / 2)) & 1);              // trama da estopa
                im.set(x, y, c, c * 0.88f, c * 0.66f);
            } else {
                float e = std::min(std::min(du, 1.0f - du) * (float)N / cols, std::min(dv, 1.0f - dv) * (float)N / rows);
                c = 0.66f + 0.16f * (tone - 0.5f) + 0.12f * (fbm(u, v, 24, 3, seed + 3u) - 0.5f);
                if (e < 1.5f) c *= 0.62f; else if (e < 3.0f) c *= 0.86f;            // juntas
                float pl = smooth01((fbm(u, v, 4, 4, seed + 7u) - 0.54f) / 0.06f);  // onde o reboco ainda esta de pe
                c += (0.76f + 0.08f * (fbm(u, v, 32, 2, seed + 9u) - 0.5f) - c) * pl;
                im.set(x, y, c * 1.04f, c * 0.82f, c * 0.60f);
            }
        }
}
static void genAdobe(Img& im) { genCourses(im, 4, 8, false, 601u); }
static void genSandbag(Img& im) { genCourses(im, 3, 4, true, 701u); }

// Sala branca: branco quase liso.
static void genWhite(Img& im) {
    const int N = im.w;
    for (int y = 0; y < N; ++y)
        for (int x = 0; x < N; ++x) {
            float c = 0.97f + 0.03f * fbm((x + 0.5f) / N, (y + 0.5f) / N, 6, 3, 501u);
            im.set(x, y, c, c, c);
        }
}

static GLuint textureId(int id) {   // 0 tecido, 1 fitas do colete, 2 clarao do tiro, 3 mancha suave
    return g_tex[id == 1 ? TX_WEB : (id == 2 ? TX_FLASH : (id == 3 ? TX_GLOW : TX_FABRIC))];
}

static void initTextures() {
    const char* ext = (const char*)glGetString(GL_EXTENSIONS);
    if (ext && strstr(ext, "GL_EXT_texture_filter_anisotropic")) glGetFloatv(GLX_MAX_TEXTURE_MAX_ANISOTROPY, &g_maxAniso);
    struct Spec { int id, size; void (*gen)(Img&); bool repeat; };
    static const Spec specs[NUM_TX] = {
        { TX_FLOOR, 1024, genFloor, true }, { TX_WALL, 512, genWall, true }, { TX_CONCRETE, 256, genConcrete, true },
        { TX_CRATE, 256, genCrate, true }, { TX_CONTAINER, 256, genContainer, true }, { TX_BARRIER, 256, genBarrier, true },
        { TX_HAZARD, 256, genHazard, true }, { TX_CEIL, 256, genCeil, true }, { TX_PAINT, 128, genPaint, true },
        { TX_GLOW, 128, genGlow, false }, { TX_FLASH, 128, genFlash, false }, { TX_FABRIC, 128, genFabric, true },
        { TX_WEB, 128, genWeb, true },
        { TX_SAND, 1024, genSand, true }, { TX_ROCK, 512, genRock, true }, { TX_ADOBE, 256, genAdobe, true },
        { TX_SANDBAG, 256, genSandbag, true }, { TX_WHITE, 128, genWhite, true },
    };
    for (int i = 0; i < NUM_TX; ++i) {
        Img im(specs[i].size, specs[i].size);
        specs[i].gen(im);
        g_tex[specs[i].id] = uploadTexture(im, specs[i].repeat);
    }
    glBindTexture(GL_TEXTURE_2D, 0);
}

// ---------------------------------------------------------------- cenario com luz assada
// O cenario nao muda durante a partida, entao a luz e calculada uma vez por mapa: cada ponto do piso, das paredes e das
// caixas soma a luz das luminarias do teto que consegue "ver" (as que estao tapadas viram sombra) mais uma luz
// ambiente que escurece em cantos e junto as bases. O resultado fica guardado nas cores dos vertices.
static const int   LIGHT_NX = 8, LIGHT_NZ = 3;           // luminarias: 8 colunas x 3 fileiras
static const float LIGHT_DX = 18.0f, LIGHT_DZ = 36.0f;   // espacamento entre colunas e entre fileiras
static float lightX(int ix) { return ((float)ix - (LIGHT_NX - 1) * 0.5f) * LIGHT_DX; }
static float lightZ(int iz) { return ((float)iz - (LIGHT_NZ - 1) * 0.5f) * LIGHT_DZ; }
// Uma luminaria so ilumina ate LIGHT_REACH de distancia no chao (de longe quase nao chega luz mesmo): 1 perto, 0 no fim.
static const float LIGHT_REACH = 48.0f;
static float lightReach(float dx, float dz) {
    float d = std::sqrt(dx * dx + dz * dz);
    return d >= LIGHT_REACH ? 0.0f : (d <= LIGHT_REACH - 12.0f ? 1.0f : (LIGHT_REACH - d) / 12.0f);
}
static const float LIGHT_HX = 5.0f, LIGHT_HZ = 1.2f;     // meio comprimento e meia largura de cada painel
static const float LIGHT_POWER = 31.0f;                  // ja conta os 5 pontos de amostra por luminaria

struct LSample { float p[3]; };
static std::vector<LSample> g_lsamples;                  // 5 pontos por luminaria: sombras de borda suave

struct BVert { float p[3], uv[2], c[3]; };
static std::vector<BVert> g_bq[NUM_TX];                  // quadrilateros do cenario, separados por textura
static GLuint g_worldList = 0, g_paintList = 0;

// O segmento a -> a + d bate em alguma caixa (a partir da caixa "first")? planes: chao e teto tambem contam.
static bool segBlocked(const float a[3], const float d[3], int first, bool planes) {
    if (planes) { float ey = a[1] + d[1]; if (ey < 0.0f || ey > g_ceil) return true; }
    const float x0 = std::min(a[0], a[0] + d[0]), x1 = std::max(a[0], a[0] + d[0]);
    const float z0 = std::min(a[2], a[2] + d[2]), z1 = std::max(a[2], a[2] + d[2]);
    for (int i = first; i < g_numObs; ++i) {
        const Obstacle& o = g_obs[i];
        if (o.x - o.hw > x1 || o.x + o.hw < x0 || o.z - o.hd > z1 || o.z + o.hd < z0) continue;   // longe do segmento
        float lo[3] = { o.x - o.hw, 0.0f, o.z - o.hd }, hi[3] = { o.x + o.hw, o.h, o.z + o.hd };
        float t0 = 0.0f, t1 = 1.0f;
        bool ok = true;
        for (int k = 0; k < 3; ++k) {
            if (std::fabs(d[k]) < 1e-7f) {
                if (a[k] < lo[k] || a[k] > hi[k]) { ok = false; break; }
            } else {
                float ta = (lo[k] - a[k]) / d[k], tb = (hi[k] - a[k]) / d[k];
                if (ta > tb) { float tmp = ta; ta = tb; tb = tmp; }
                if (ta > t0) t0 = ta;
                if (tb < t1) t1 = tb;
                if (t0 > t1) { ok = false; break; }
            }
        }
        if (ok) return true;
    }
    return false;
}

// Deserto: a luz vem do sol (uma direcao so) e do ceu.
static const float SUN_DIR[3] = { 0.42f, 0.80f, 0.43f };
static const float SUN_FAR = 400.0f;

// Fracao das direcoes em volta do ponto (do lado da normal) tapadas por algo a menos de 2,4: cantos e bases ficam escuros.
static float bakeOcclusion(const float q[3], const float n[3]) {
    static const float D = 0.5774f;
    static const float dirs[14][3] = { { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 },
        { D, D, D }, { -D, D, D }, { D, -D, D }, { -D, -D, D }, { D, D, -D }, { -D, D, -D }, { D, -D, -D }, { -D, -D, -D } };
    int used = 0, occ = 0;
    for (int i = 0; i < 14; ++i) {
        if (n[0] * dirs[i][0] + n[1] * dirs[i][1] + n[2] * dirs[i][2] < 0.15f) continue;
        float d[3] = { dirs[i][0] * 2.4f, dirs[i][1] * 2.4f, dirs[i][2] * 2.4f };
        ++used;
        if (segBlocked(q, d, 0, true)) ++occ;
    }
    return used ? (float)occ / (float)used : 0.0f;
}

// Luz que chega num ponto da superficie (p, normal n). Galpao: direta das luminarias + ambiente com oclusao. Deserto:
// sol (com a sombra das pecas) + ceu. Sala branca: quase tudo igual, so um tom por lado e os cantos um pouco mais escuros.
static void bakeLight(const float p[3], const float n[3], float out[3]) {
    float q[3] = { p[0] + n[0] * 0.03f, p[1] + n[1] * 0.03f, p[2] + n[2] * 0.03f };
    float occ = bakeOcclusion(q, n);
    if (g_worldMap == MAP_BRANCO) {
        float c = n[1] > 0.5f ? 0.98f : (n[1] < -0.5f ? 0.955f : (std::fabs(n[0]) > 0.5f ? 0.955f : 0.935f));
        out[0] = out[1] = out[2] = c * (1.0f - 0.13f * occ);
        return;
    }
    if (g_worldMap == MAP_DESERTO) {
        float ndl = n[0] * SUN_DIR[0] + n[1] * SUN_DIR[1] + n[2] * SUN_DIR[2], direct = 0.0f;
        if (ndl > 0.0f) {
            float L[3] = { SUN_DIR[0] * SUN_FAR, SUN_DIR[1] * SUN_FAR, SUN_DIR[2] * SUN_FAR };
            if (!segBlocked(q, L, 0, false)) direct = ndl;
        }
        float amb = 0.46f * (1.0f - 0.6f * occ);
        out[0] = 0.80f * direct + 0.86f * amb;    // sol quente, sombra azulada (a luz do ceu)
        out[1] = 0.74f * direct + 0.88f * amb;
        out[2] = 0.62f * direct + 1.00f * amb;
        return;
    }
    float direct = 0.0f;
    for (size_t i = 0; i < g_lsamples.size(); ++i) {
        float L[3] = { g_lsamples[i].p[0] - q[0], g_lsamples[i].p[1] - q[1], g_lsamples[i].p[2] - q[2] };
        float reach = lightReach(L[0], L[2]);
        if (reach <= 0.0f) continue;
        float d2 = L[0] * L[0] + L[1] * L[1] + L[2] * L[2], inv = 1.0f / std::sqrt(d2);
        float ndl = (n[0] * L[0] + n[1] * L[1] + n[2] * L[2]) * inv, cl = L[1] * inv;   // cl: o painel ilumina para baixo
        ndl = (ndl + 0.30f) / 1.30f;              // luz "envolvente": imita a que rebate no piso e nas paredes
        if (ndl <= 0.0f || cl <= 0.0f) continue;
        if (segBlocked(q, L, NUM_WALLS, false)) continue;
        direct += ndl * cl / d2 * reach;
    }
    direct *= LIGHT_POWER;
    float amb = 0.34f * (1.0f - 0.7f * occ);
    out[0] = 1.00f * direct + 0.80f * amb;        // luz direta quente, ambiente frio
    out[1] = 0.95f * direct + 0.88f * amb;
    out[2] = 0.86f * direct + 1.00f * amb;
}

// Teto e vigas nao recebem luz direta (as luminarias apontam para baixo): so um clarao em volta de cada painel.
static void ceilLight(const float p[3], float out[3]) {
    float best = 1e9f;
    for (int iz = 0; iz < LIGHT_NZ; ++iz)
        for (int ix = 0; ix < LIGHT_NX; ++ix) {
            float dx = std::max(std::fabs(p[0] - lightX(ix)) - LIGHT_HX, 0.0f);
            float dz = std::max(std::fabs(p[2] - lightZ(iz)) - LIGHT_HZ, 0.0f);
            best = std::min(best, std::sqrt(dx * dx + dz * dz + (ROOM_H - p[1]) * (ROOM_H - p[1])));
        }
    float c = 0.34f + 0.62f * std::exp(-best / 2.6f);
    out[0] = c; out[1] = c * 0.97f; out[2] = c * 0.92f;
}

// Um retangulo do cenario (origem o, lados e1 x e2), dividido em quadradinhos de ~step para a luz variar ao longo dele.
static void emitFace(int tex, const float o[3], const float e1[3], float l1, const float e2[3], float l2, const float n[3],
                     float u0, float du, float v0, float dv, const float tint[3], float step, bool ceiling = false) {
    int n1 = std::max(1, (int)std::ceil(l1 / step - 1e-3f)), n2 = std::max(1, (int)std::ceil(l2 / step - 1e-3f));
    std::vector<BVert> node((size_t)(n1 + 1) * (n2 + 1));
    for (int j = 0; j <= n2; ++j)
        for (int i = 0; i <= n1; ++i) {
            BVert& b = node[(size_t)j * (n1 + 1) + i];
            float a = l1 * i / n1, t = l2 * j / n2;
            for (int k = 0; k < 3; ++k) b.p[k] = o[k] + e1[k] * a + e2[k] * t;
            b.uv[0] = u0 + du * a; b.uv[1] = v0 + dv * t;
            if (ceiling) ceilLight(b.p, b.c); else bakeLight(b.p, n, b.c);
            for (int k = 0; k < 3; ++k) b.c[k] = std::min(b.c[k] * tint[k], 1.0f);
        }
    std::vector<BVert>& out = g_bq[tex];
    for (int j = 0; j < n2; ++j)
        for (int i = 0; i < n1; ++i) {
            out.push_back(node[(size_t)j * (n1 + 1) + i]);
            out.push_back(node[(size_t)j * (n1 + 1) + i + 1]);
            out.push_back(node[(size_t)(j + 1) * (n1 + 1) + i + 1]);
            out.push_back(node[(size_t)(j + 1) * (n1 + 1) + i]);
        }
}

// Os quatro lados de uma caixa entre as alturas y0 e y1, com a textura repetindo a cada (1/du, 1/dv) unidades.
static void emitSides(int tex, float x, float z, float hw, float hd, float y0, float y1, float du, float dv,
                      const float tint[3], float step) {
    static const float UP[3] = { 0, 1, 0 };
    const float face[4][7] = {      // origem (x, z), direcao do lado (x, z), normal (x, z), comprimento
        { x - hw, z + hd,  1, 0,  0,  1, 2 * hw }, { x + hw, z - hd, -1, 0,  0, -1, 2 * hw },
        { x + hw, z + hd,  0, -1, 1,  0, 2 * hd }, { x - hw, z - hd,  0, 1, -1,  0, 2 * hd } };
    for (int f = 0; f < 4; ++f) {
        float o[3] = { face[f][0], y0, face[f][1] }, e1[3] = { face[f][2], 0, face[f][3] }, n[3] = { face[f][4], 0, face[f][5] };
        emitFace(tex, o, e1, face[f][6], UP, y1 - y0, n, 0.0f, du, y0 * dv, dv, tint, step);
    }
}

static void emitTop(int tex, const Obstacle& b, float du, float dv, const float tint[3], float step) {
    float o[3] = { b.x - b.hw, b.h, b.z - b.hd }, e1[3] = { 1, 0, 0 }, e2[3] = { 0, 0, 1 }, n[3] = { 0, 1, 0 };
    emitFace(tex, o, e1, 2 * b.hw, e2, 2 * b.hd, n, 0.0f, du, 0.0f, dv, tint, step);
}

static GLuint compileBatches(int from, int to) {
    GLuint list = glGenLists(1);
    glNewList(list, GL_COMPILE);
    for (int t = from; t <= to; ++t) {
        if (g_bq[t].empty()) continue;
        glBindTexture(GL_TEXTURE_2D, g_tex[t]);
        glBegin(GL_QUADS);
        for (size_t i = 0; i < g_bq[t].size(); ++i) {
            const BVert& b = g_bq[t][i];
            glColor3fv(b.c);
            glTexCoord2fv(b.uv);
            glVertex3fv(b.p);
        }
        glEnd();
        std::vector<BVert>().swap(g_bq[t]);
    }
    glEndList();
    return list;
}

// Uma caixa de rocha que so enfeita (nao e obstaculo): beirada da muralha e morros ao longe.
static void emitRockBox(float x, float z, float hw, float hd, float y0, float h, const float tint[3], float step) {
    Obstacle b = { x, z, hw, hd, h, 1.0f, 1.0f, 1.0f, OB_ROCK };
    const float top[3] = { tint[0] * 0.98f, tint[1] * 0.86f, tint[2] * 0.80f };
    emitSides(TX_ROCK, x, z, hw, hd, y0, h, 1.0f / 8.0f, 1.0f / 8.0f, tint, step);
    emitTop(TX_SAND, b, 0.1f, 0.1f, top, step);
}

// Deserto: o que fica fora da arena. Areia ate o horizonte, pedras de alturas variadas em cima da muralha (a beirada
// fica irregular, como um paredao de verdade) e morros de topo chato ao longe.
static void buildDesertScenery() {
    const float X[3] = { 1, 0, 0 }, Y[3] = { 0, 1, 0 }, Z[3] = { 0, 0, 1 };
    const float R = 640.0f, ex = ROOM_X + 6.0f, ez = ROOM_Z + 6.0f, sand[3] = { 1.0f, 0.99f, 0.97f };
    {
        float o1[3] = { -R, 0, -R }, o2[3] = { -R, 0, ez }, o3[3] = { -R, 0, -ez }, o4[3] = { ex, 0, -ez };
        emitFace(TX_SAND, o1, X, 2 * R, Z, R - ez, Y, 0.0f, 0.1f, 0.0f, 0.1f, sand, 40.0f);
        emitFace(TX_SAND, o2, X, 2 * R, Z, R - ez, Y, 0.0f, 0.1f, 0.0f, 0.1f, sand, 40.0f);
        emitFace(TX_SAND, o3, X, R - ex, Z, 2 * ez, Y, 0.0f, 0.1f, 0.0f, 0.1f, sand, 40.0f);
        emitFace(TX_SAND, o4, X, R - ex, Z, 2 * ez, Y, 0.0f, 0.1f, 0.0f, 0.1f, sand, 40.0f);
    }
    unsigned rs = 4242u;                                    // sorteio proprio: o cenario e sempre o mesmo
    auto rnd = [&rs]() { rs = rs * 1664525u + 1013904223u; return (float)((rs >> 8) & 0xFFFF) / 65536.0f; };
    for (int side = 0; side < 4; ++side) {                  // pedras em cima da muralha, pelos quatro lados
        float len = side < 2 ? ROOM_X + 5.0f : ROOM_Z + 1.0f, a = -len;
        while (a < len) {
            float hw = 2.0f + rnd() * 3.2f, hd = 1.5f + rnd() * 1.1f, h = 8.2f + rnd() * 5.8f, c = 0.86f + rnd() * 0.18f;
            float gap = rnd() < 0.3f ? rnd() * 5.0f : 0.0f;
            float along = std::min(a + hw, len - 0.5f), out = 3.4f + (rnd() - 0.5f) * 0.6f;
            const float tint[3] = { c, c * (0.95f + rnd() * 0.05f), c * (0.92f + rnd() * 0.08f) };
            if (side == 0)      emitRockBox(along, -ROOM_Z - out, hw, hd, DESERT_WALL_H - 0.5f, h, tint, 1.6f);
            else if (side == 1) emitRockBox(along,  ROOM_Z + out, hw, hd, DESERT_WALL_H - 0.5f, h, tint, 1.6f);
            else if (side == 2) emitRockBox(-ROOM_X - out, along, hd, hw, DESERT_WALL_H - 0.5f, h, tint, 1.6f);
            else                emitRockBox( ROOM_X + out, along, hd, hw, DESERT_WALL_H - 0.5f, h, tint, 1.6f);
            a += 2.0f * hw - 0.4f + gap;
        }
    }
    static const float mesa[][5] = {                        // x, z, meia largura, meia profundidade, altura
        { -190.0f, -160.0f, 45.0f, 30.0f, 42.0f }, {  -40.0f, -240.0f, 60.0f, 25.0f, 30.0f }, { 130.0f, -215.0f, 35.0f, 28.0f, 55.0f },
        {  255.0f,  -90.0f, 30.0f, 50.0f, 38.0f }, {  235.0f,  115.0f, 55.0f, 30.0f, 48.0f }, {  60.0f,  245.0f, 40.0f, 25.0f, 34.0f },
        { -145.0f,  225.0f, 50.0f, 30.0f, 60.0f }, { -265.0f,   40.0f, 30.0f, 55.0f, 36.0f }, { -340.0f, -210.0f, 70.0f, 40.0f, 70.0f },
        {  350.0f,  270.0f, 60.0f, 60.0f, 80.0f }, {   10.0f, -390.0f, 90.0f, 30.0f, 50.0f }, { -60.0f,  400.0f, 80.0f, 35.0f, 65.0f } };
    for (size_t i = 0; i < sizeof(mesa) / sizeof(mesa[0]); ++i) {
        const float* m = mesa[i];
        float c = 0.90f + rnd() * 0.14f;
        const float tint[3] = { c, c * 0.97f, c * 0.95f };
        emitRockBox(m[0], m[1], m[2], m[3], 0.0f, m[4], tint, 14.0f);                                   // a base
        emitRockBox(m[0] + (rnd() - 0.5f) * m[2] * 0.4f, m[1] + (rnd() - 0.5f) * m[3] * 0.4f,           // um degrau em cima
                    m[2] * (0.45f + rnd() * 0.2f), m[3] * (0.45f + rnd() * 0.2f), m[4], m[4] * (1.3f + rnd() * 0.3f), tint, 14.0f);
    }
}

// Calcula a luz e guarda o cenario do mapa g_worldMap (as caixas ja estao em g_obs).
static void buildWorld() {
    const int map = g_worldMap;
    if (g_worldList) glDeleteLists(g_worldList, 1);
    if (g_paintList) glDeleteLists(g_paintList, 1);
    g_lsamples.clear();
    if (map == MAP_GALPAO)
        for (int iz = 0; iz < LIGHT_NZ; ++iz)
            for (int ix = 0; ix < LIGHT_NX; ++ix)
                for (int k = -2; k <= 2; ++k) {
                    LSample s = { { lightX(ix) + k * 2.0f, ROOM_H - 0.1f, lightZ(iz) } };
                    g_lsamples.push_back(s);
                }

    const float W[3] = { 1, 1, 1 }, X[3] = { 1, 0, 0 }, Y[3] = { 0, 1, 0 }, Z[3] = { 0, 0, 1 }, NX[3] = { -1, 0, 0 }, NZ[3] = { 0, 0, -1 };
    const float DOWN[3] = { 0, -1, 0 };

    {   // piso
        float o[3] = { -ROOM_X, 0, -ROOM_Z };
        int tex = map == MAP_DESERTO ? TX_SAND : (map == MAP_BRANCO ? TX_WHITE : TX_FLOOR);
        emitFace(tex, o, X, 2 * ROOM_X, Z, 2 * ROOM_Z, Y, 0.0f, 0.1f, 0.0f, 0.1f, W, 0.75f);
    }
    if (map == MAP_DESERTO) {
        // muralha de rocha (os lados e o topo de cada trecho) e o que se ve por cima dela
        const float top[3] = { 0.98f, 0.86f, 0.80f };
        for (int i = 0; i < NUM_WALLS; ++i) {
            const Obstacle& b = g_obs[i];
            emitSides(TX_ROCK, b.x, b.z, b.hw, b.hd, 0.0f, b.h, 1.0f / 8.0f, 1.0f / 8.0f, W, 1.0f);
            emitTop(TX_SAND, b, 0.1f, 0.1f, top, 1.5f);
        }
        buildDesertScenery();
    } else {
        // paredes. Galpao: barra pintada embaixo, filete amarelo e concreto aparente ate o teto. Sala branca: lisas.
        const float band[3] = { 0.42f, 0.50f, 0.62f }, line[3] = { 1.25f, 0.95f, 0.22f };
        const float y[4] = { 0.0f, 1.3f, 1.42f, ROOM_H };
        const float* tint[3] = { band, line, W };
        const bool white = map == MAP_BRANCO;
        for (int s = white ? 2 : 0; s < 3; ++s) {
            float y0 = white ? 0.0f : y[s];
            float o1[3] = { -ROOM_X, y0, -ROOM_Z }, o2[3] = { ROOM_X, y0, ROOM_Z }, o3[3] = { -ROOM_X, y0, ROOM_Z }, o4[3] = { ROOM_X, y0, -ROOM_Z };
            float h = y[s + 1] - y0, v0 = y0 / 8.0f;
            int tex = white ? TX_WHITE : TX_WALL;
            emitFace(tex, o1, X, 2 * ROOM_X, Y, h, Z, 0.0f, 0.125f, v0, 0.125f, tint[s], 1.0f);
            emitFace(tex, o2, NX, 2 * ROOM_X, Y, h, NZ, 0.0f, 0.125f, v0, 0.125f, tint[s], 1.0f);
            emitFace(tex, o3, NZ, 2 * ROOM_Z, Y, h, X, 0.0f, 0.125f, v0, 0.125f, tint[s], 1.0f);
            emitFace(tex, o4, Z, 2 * ROOM_Z, Y, h, NX, 0.0f, 0.125f, v0, 0.125f, tint[s], 1.0f);
        }
        float o[3] = { -ROOM_X, ROOM_H, -ROOM_Z };          // teto
        if (white) emitFace(TX_WHITE, o, X, 2 * ROOM_X, Z, 2 * ROOM_Z, DOWN, 0.0f, 0.1f, 0.0f, 0.1f, W, 2.5f);
        else {
            emitFace(TX_CEIL, o, X, 2 * ROOM_X, Z, 2 * ROOM_Z, DOWN, 0.0f, 1.0f / 6.0f, 0.0f, 1.0f / 6.0f, W, 2.5f, true);
            const float steel[3] = { 0.62f, 0.64f, 0.70f };
            for (int g = 0; g < LIGHT_NX - 1; ++g) {        // uma viga entre cada par de colunas de luminarias
                float gx = lightX(g) + LIGHT_DX * 0.5f, y0 = ROOM_H - 0.9f;
                float a[3] = { gx - 0.3f, y0, -ROOM_Z }, b[3] = { gx + 0.3f, y0, -ROOM_Z };
                emitFace(TX_CEIL, a, Z, 2 * ROOM_Z, Y, 0.9f, NX, 0.0f, 0.25f, 0.0f, 0.25f, steel, 2.5f, true);
                emitFace(TX_CEIL, b, Z, 2 * ROOM_Z, Y, 0.9f, X, 0.0f, 0.25f, 0.0f, 0.25f, steel, 2.5f, true);
                emitFace(TX_CEIL, a, Z, 2 * ROOM_Z, X, 0.6f, DOWN, 0.0f, 0.25f, 0.0f, 0.25f, steel, 2.5f, true);
            }
        }
    }
    for (int i = NUM_WALLS; i < g_numObs; ++i) {
        const Obstacle& b = g_obs[i];
        if (map == MAP_BRANCO) {
            emitSides(TX_WHITE, b.x, b.z, b.hw, b.hd, 0.0f, b.h, 0.25f, 0.25f, W, 0.8f);
            if (b.h < ROOM_H - 0.1f) emitTop(TX_WHITE, b, 0.25f, 0.25f, W, 0.8f);
            continue;
        }
        const float tone[3] = { b.r, b.g, b.b };
        if (map == MAP_DESERTO && (b.kind == OB_ROCK || b.kind == OB_PILLAR)) {
            const float top[3] = { b.r * 0.98f, b.g * 0.86f, b.b * 0.80f };
            emitSides(TX_ROCK, b.x, b.z, b.hw, b.hd, 0.0f, b.h, 1.0f / 8.0f, 1.0f / 8.0f, tone, 0.7f);
            emitTop(TX_SAND, b, 0.1f, 0.1f, top, 0.7f);
            continue;
        }
        if (map == MAP_DESERTO && b.kind == OB_COVER) {
            emitSides(TX_ADOBE, b.x, b.z, b.hw, b.hd, 0.0f, b.h, 0.25f, 0.25f, tone, 0.8f);
            emitTop(TX_ADOBE, b, 0.25f, 0.25f, tone, 0.8f);
            continue;
        }
        if (map == MAP_DESERTO && b.kind == OB_BARRIER) {
            emitSides(TX_SANDBAG, b.x, b.z, b.hw, b.hd, 0.0f, b.h, 1.0f / 3.0f, 1.0f / b.h, W, 0.5f);
            emitTop(TX_SANDBAG, b, 1.0f / 3.0f, 1.0f / (2.0f * std::min(b.hw, b.hd)), W, 0.5f);
            continue;
        }
        switch (b.kind) {
        case OB_PILLAR: {
            const float t[3] = { 0.92f, 0.92f, 0.96f };
            emitSides(TX_HAZARD, b.x, b.z, b.hw, b.hd, 0.0f, 1.1f, 1.0f / 2.2f, 1.0f / 2.2f, W, 0.55f);
            emitSides(TX_CONCRETE, b.x, b.z, b.hw, b.hd, 1.1f, b.h, 0.25f, 0.25f, t, 1.0f);
            break;
        }
        case OB_COVER: {
            emitSides(TX_CONCRETE, b.x, b.z, b.hw, b.hd, 0.0f, b.h, 0.25f, 0.25f, W, 0.8f);
            emitTop(TX_CONCRETE, b, 0.25f, 0.25f, W, 0.8f);
            break;
        }
        case OB_CONTAINER: {
            const float t[3] = { b.r * 1.45f, b.g * 1.45f, b.b * 1.45f };
            emitSides(TX_CONTAINER, b.x, b.z, b.hw, b.hd, 0.0f, b.h, 1.0f / 2.4f, 1.0f / b.h, t, 0.8f);
            emitTop(TX_CONTAINER, b, 1.0f / 2.4f, 1.0f / (2.0f * std::min(b.hw, b.hd)), t, 0.8f);
            break;
        }
        case OB_CRATE: {
            float f = b.r / 0.55f;
            const float t[3] = { f, f, f };
            emitSides(TX_CRATE, b.x, b.z, b.hw, b.hd, 0.0f, b.h, 1.0f / (2.0f * b.hw), 1.0f / b.h, t, 0.6f);
            emitTop(TX_CRATE, b, 1.0f / (2.0f * b.hw), 1.0f / (2.0f * b.hd), t, 0.6f);
            break;
        }
        default: {      // barreira baixa
            emitSides(TX_BARRIER, b.x, b.z, b.hw, b.hd, 0.0f, b.h, 1.0f / 3.0f, 1.0f / b.h, W, 0.5f);
            emitTop(TX_CONCRETE, b, 0.25f, 0.25f, W, 0.5f);
            break;
        }
        }
    }
    for (int ring = 0; ring < (map == MAP_GALPAO ? 2 : 0); ++ring) {   // galpao: faixas amarelas no piso, dois retangulos
        const float lx = ring ? 61.0f : 31.0f, lz = ring ? 45.0f : 25.0f, w = 0.3f;
        const float seg[4][4] = { { -lx, -lz, 2 * lx, 0 }, { -lx, lz - w, 2 * lx, 0 }, { -lx, -lz, 0, 2 * lz }, { lx - w, -lz, 0, 2 * lz } };
        for (int s = 0; s < 4; ++s) {
            float o[3] = { seg[s][0], 0.012f, seg[s][1] };
            if (seg[s][2] > 0.0f) emitFace(TX_PAINT, o, X, seg[s][2], Z, w, Y, 0.0f, 0.25f, 0.0f, 1.0f / w, W, 1.0f);
            else                  emitFace(TX_PAINT, o, Z, seg[s][3], X, w, Y, 0.0f, 0.25f, 0.0f, 1.0f / w, W, 1.0f);
        }
    }
    g_paintList = compileBatches(TX_PAINT, TX_PAINT);
    g_worldList = compileBatches(0, NUM_TX - 1);
}

// Ceu do deserto: uma cupula em volta da camera (azul no alto, clara e amarelada no horizonte) e o sol.
static void drawSky() {
    float mv[16], e[3];
    glGetFloatv(GL_MODELVIEW_MATRIX, mv);
    for (int i = 0; i < 3; ++i) e[i] = -(mv[i * 4] * mv[12] + mv[i * 4 + 1] * mv[13] + mv[i * 4 + 2] * mv[14]);
    glPushAttrib(GL_ENABLE_BIT | GL_DEPTH_BUFFER_BIT | GL_COLOR_BUFFER_BIT);
    glDisable(GL_FOG);
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glDisable(GL_TEXTURE_2D);
    static const float elev[5] = { -0.40f, 0.0f, 0.17f, 0.62f, 1.5708f };
    static const float col[5][3] = { { 0.90f, 0.83f, 0.71f }, { 0.90f, 0.83f, 0.71f }, { 0.70f, 0.80f, 0.90f },
                                     { 0.36f, 0.57f, 0.88f }, { 0.20f, 0.42f, 0.80f } };
    const int SEG = 24;
    const float R = 300.0f;
    for (int b = 0; b < 4; ++b) {
        glBegin(GL_QUAD_STRIP);
        for (int s = 0; s <= SEG; ++s) {
            float a = 2.0f * PI * (float)(s % SEG) / (float)SEG, ca = std::cos(a), sa = std::sin(a);
            for (int k = 1; k >= 0; --k) {
                float el = elev[b + k], ce = std::cos(el);
                glColor3fv(col[b + k]);
                glVertex3f(e[0] + R * ce * ca, e[1] + R * std::sin(el), e[2] + R * ce * sa);
            }
        }
        glEnd();
    }
    const float c[3] = { e[0] + SUN_DIR[0] * 280.0f, e[1] + SUN_DIR[1] * 280.0f, e[2] + SUN_DIR[2] * 280.0f };
    const float r[3] = { mv[0], mv[4], mv[8] }, u[3] = { mv[1], mv[5], mv[9] };
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, g_tex[TX_GLOW]);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE);
    glBegin(GL_QUADS);
    for (int pass = 0; pass < 3; ++pass) {                  // halo largo e o disco no meio
        float s = pass == 0 ? 85.0f : 15.0f;
        glColor4f(1.0f, pass == 0 ? 0.92f : 0.98f, pass == 0 ? 0.72f : 0.90f, pass == 0 ? 0.50f : 1.0f);
        glTexCoord2f(0, 0); glVertex3f(c[0] - (r[0] + u[0]) * s, c[1] - (r[1] + u[1]) * s, c[2] - (r[2] + u[2]) * s);
        glTexCoord2f(1, 0); glVertex3f(c[0] + (r[0] - u[0]) * s, c[1] + (r[1] - u[1]) * s, c[2] + (r[2] - u[2]) * s);
        glTexCoord2f(1, 1); glVertex3f(c[0] + (r[0] + u[0]) * s, c[1] + (r[1] + u[1]) * s, c[2] + (r[2] + u[2]) * s);
        glTexCoord2f(0, 1); glVertex3f(c[0] - (r[0] - u[0]) * s, c[1] - (r[1] - u[1]) * s, c[2] - (r[2] - u[2]) * s);
    }
    glEnd();
    glPopAttrib();
}

// O cenario inteiro: (ceu,) pecas com a luz ja calculada, faixas do piso e os paineis das luminarias do galpao.
static void drawWorld() {
    glDisable(GL_LIGHTING);
    if (g_worldMap == MAP_DESERTO) drawSky();
    glEnable(GL_TEXTURE_2D);
    glCallList(g_worldList);

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDepthMask(GL_FALSE);
    glCallList(g_paintList);
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
    glDisable(GL_TEXTURE_2D);

    if (g_worldMap == MAP_GALPAO) {
        glBegin(GL_QUADS);
        for (int iz = 0; iz < LIGHT_NZ; ++iz)
            for (int ix = 0; ix < LIGHT_NX; ++ix) {
                float cx = lightX(ix), cz = lightZ(iz), y = ROOM_H - 0.06f;
                glColor3f(0.10f, 0.10f, 0.12f);                 // caixa da luminaria
                glVertex3f(cx - LIGHT_HX - 0.25f, y + 0.02f, cz - LIGHT_HZ - 0.25f); glVertex3f(cx + LIGHT_HX + 0.25f, y + 0.02f, cz - LIGHT_HZ - 0.25f);
                glVertex3f(cx + LIGHT_HX + 0.25f, y + 0.02f, cz + LIGHT_HZ + 0.25f); glVertex3f(cx - LIGHT_HX - 0.25f, y + 0.02f, cz + LIGHT_HZ + 0.25f);
                glColor3f(1.0f, 0.97f, 0.86f);                  // painel aceso
                glVertex3f(cx - LIGHT_HX, y, cz - LIGHT_HZ); glVertex3f(cx + LIGHT_HX, y, cz - LIGHT_HZ);
                glVertex3f(cx + LIGHT_HX, y, cz + LIGHT_HZ); glVertex3f(cx - LIGHT_HX, y, cz + LIGHT_HZ);
            }
        glEnd();
    }
    glEnable(GL_LIGHTING);
}

// Halo em volta das luminarias do galpao (depois de tudo que e solido, somando luz).
static void drawLightGlow() {
    if (g_worldMap != MAP_GALPAO) return;
    glDisable(GL_LIGHTING);
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, g_tex[TX_GLOW]);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE);
    glDepthMask(GL_FALSE);
    glColor4f(1.0f, 0.93f, 0.78f, 0.30f);
    glBegin(GL_QUADS);
    for (int iz = 0; iz < LIGHT_NZ; ++iz)
        for (int ix = 0; ix < LIGHT_NX; ++ix) {
            float cx = lightX(ix), cz = lightZ(iz), y = ROOM_H - 0.12f, gx = LIGHT_HX + 2.6f, gz = LIGHT_HZ + 2.6f;
            glTexCoord2f(0, 0); glVertex3f(cx - gx, y, cz - gz);
            glTexCoord2f(1, 0); glVertex3f(cx + gx, y, cz - gz);
            glTexCoord2f(1, 1); glVertex3f(cx + gx, y, cz + gz);
            glTexCoord2f(0, 1); glVertex3f(cx - gx, y, cz + gz);
        }
    glEnd();
    glDepthMask(GL_TRUE);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_BLEND);
    glDisable(GL_TEXTURE_2D);
    glEnable(GL_LIGHTING);
}

// ---- luz nos personagens: de onde vem e quanta luz chega no lugar onde ele esta (na sombra de um muro fica escuro)
struct Probe { float dir[3]; float direct; };

static void lightProbe(float x, float y, float z, Probe& pr) {
    float q[3] = { x, y + 1.7f, z }, sum[3] = { 0, 0, 0 };
    if (g_worldMap == MAP_DESERTO) {                        // sol: ou bate, ou esta na sombra de alguma peca
        float L[3] = { SUN_DIR[0] * SUN_FAR, SUN_DIR[1] * SUN_FAR, SUN_DIR[2] * SUN_FAR };
        for (int k = 0; k < 3; ++k) pr.dir[k] = SUN_DIR[k];
        pr.direct = segBlocked(q, L, 0, false) ? 0.12f : 1.0f;
        return;
    }
    if (g_worldMap == MAP_BRANCO) {                         // luz igual em todo lugar, vinda de cima
        pr.dir[0] = 0.20f; pr.dir[1] = 0.95f; pr.dir[2] = 0.24f;
        pr.direct = 0.50f;
        return;
    }
    for (size_t i = 0; i < g_lsamples.size(); ++i) {
        float L[3] = { g_lsamples[i].p[0] - q[0], g_lsamples[i].p[1] - q[1], g_lsamples[i].p[2] - q[2] };
        float reach = lightReach(L[0], L[2]);
        if (reach <= 0.0f) continue;
        float d2 = L[0] * L[0] + L[1] * L[1] + L[2] * L[2], inv = 1.0f / std::sqrt(d2), cl = L[1] * inv;
        if (cl <= 0.0f || segBlocked(q, L, NUM_WALLS, false)) continue;
        float w = cl / d2 * inv * reach;
        for (int k = 0; k < 3; ++k) sum[k] += L[k] * w;
    }
    float len = std::sqrt(sum[0] * sum[0] + sum[1] * sum[1] + sum[2] * sum[2]);
    pr.direct = std::min(len * LIGHT_POWER, 1.15f);
    if (len > 1e-6f) { pr.dir[0] = sum[0] / len; pr.dir[1] = sum[1] / len; pr.dir[2] = sum[2] / len; }
    else { pr.dir[0] = 0.0f; pr.dir[1] = 1.0f; pr.dir[2] = 0.0f; }
}

// Chamar com a matriz da camera ativa (antes de mover para o personagem): a direcao da luz e dada no mundo.
static void applyProbe(const Probe& pr) {
    static const float AMB[NUM_MAPS][3] = { { 0.30f, 0.33f, 0.38f }, { 0.40f, 0.40f, 0.43f }, { 0.50f, 0.50f, 0.51f } };
    static const float DIF[NUM_MAPS][3] = { { 1.00f, 0.95f, 0.86f }, { 1.00f, 0.94f, 0.80f }, { 1.00f, 1.00f, 1.00f } };
    const int map = g_worldMap < 0 ? 0 : g_worldMap;
    float d = 0.25f + pr.direct;
    const float pos[4] = { pr.dir[0], pr.dir[1], pr.dir[2], 0.0f };
    const float dif[4] = { DIF[map][0] * d, DIF[map][1] * d, DIF[map][2] * d, 1.0f };
    const float amb[4] = { AMB[map][0], AMB[map][1], AMB[map][2], 1.0f };
    glLightfv(GL_LIGHT0, GL_POSITION, pos);
    glLightfv(GL_LIGHT0, GL_DIFFUSE, dif);
    glLightfv(GL_LIGHT0, GL_AMBIENT, amb);
    // luz de preenchimento, fria e fraca, do lado oposto: da volume ao corpo
    const float pos2[4] = { -pr.dir[0], 0.35f, -pr.dir[2] - 0.2f, 0.0f }, dif2[4] = { 0.16f, 0.19f, 0.25f, 1.0f }, zero[4] = { 0, 0, 0, 1 };
    glLightfv(GL_LIGHT1, GL_POSITION, pos2);
    glLightfv(GL_LIGHT1, GL_DIFFUSE, dif2);
    glLightfv(GL_LIGHT1, GL_AMBIENT, zero);
    glEnable(GL_LIGHT1);
}

// ---- particulas: faiscas, poeira e fumaca
struct Particle { float p[3], v[3], life, maxLife, size, grow, grav, r, g, b, a; bool add; };
static std::vector<Particle> g_parts;

static void spawnParticle(const float p[3], const float v[3], float life, float size, float grow, float grav,
                          float r, float g, float b, float a, bool add) {
    if (g_parts.size() >= 600) return;
    Particle q;
    for (int k = 0; k < 3; ++k) { q.p[k] = p[k]; q.v[k] = v[k]; }
    q.life = q.maxLife = life; q.size = size; q.grow = grow; q.grav = grav;
    q.r = r; q.g = g; q.b = b; q.a = a; q.add = add;
    g_parts.push_back(q);
}

// Tiro batendo em parede, caixa ou chao: faiscas que ricocheteiam e uma nuvem de poeira.
static void spawnImpact(const float p[3], const float n[3]) {
    for (int i = 0; i < 7; ++i) {
        float v[3];
        for (int k = 0; k < 3; ++k) v[k] = n[k] * (3.0f + frand() * 6.0f) + (frand() - 0.5f) * 7.0f;
        spawnParticle(p, v, 0.18f + frand() * 0.22f, 0.05f + frand() * 0.04f, -0.08f, 14.0f, 1.0f, 0.75f, 0.35f, 1.0f, true);
    }
    for (int i = 0; i < 3; ++i) {
        float v[3];
        for (int k = 0; k < 3; ++k) v[k] = n[k] * (0.8f + frand() * 1.2f) + (frand() - 0.5f) * 0.8f;
        spawnParticle(p, v, 0.5f + frand() * 0.4f, 0.16f + frand() * 0.08f, 0.9f, -0.4f, 0.62f, 0.60f, 0.56f, 0.35f, false);
    }
}

// Tiro acertando um personagem: pequeno estouro avermelhado.
static void spawnBodyHit(const float p[3]) {
    for (int i = 0; i < 8; ++i) {
        float v[3] = { (frand() - 0.5f) * 5.0f, (frand() - 0.2f) * 4.0f, (frand() - 0.5f) * 5.0f };
        spawnParticle(p, v, 0.25f + frand() * 0.2f, 0.07f + frand() * 0.05f, 0.1f, 9.0f, 0.75f, 0.08f, 0.06f, 0.85f, false);
    }
    float z[3] = { 0, 0.3f, 0 };
    spawnParticle(p, z, 0.12f, 0.35f, 2.0f, 0.0f, 1.0f, 0.35f, 0.25f, 0.6f, true);
}

static void updateParticles(float dt) {
    size_t keep = 0;
    for (size_t i = 0; i < g_parts.size(); ++i) {
        Particle q = g_parts[i];
        q.life -= dt;
        if (q.life <= 0.0f) continue;
        q.v[1] -= q.grav * dt;
        for (int k = 0; k < 3; ++k) q.p[k] += q.v[k] * dt;
        if (q.p[1] < 0.03f) { q.p[1] = 0.03f; q.v[1] = -q.v[1] * 0.35f; q.v[0] *= 0.6f; q.v[2] *= 0.6f; }   // quica no chao
        q.size = std::max(0.01f, q.size + q.grow * dt);
        g_parts[keep++] = q;
    }
    g_parts.resize(keep);
}

// Quadradinhos sempre de frente para a camera, com a textura de mancha suave.
static void drawParticles() {
    if (g_parts.empty()) return;
    float mv[16];
    glGetFloatv(GL_MODELVIEW_MATRIX, mv);
    const float R[3] = { mv[0], mv[4], mv[8] }, U[3] = { mv[1], mv[5], mv[9] };
    glPushAttrib(GL_ENABLE_BIT | GL_DEPTH_BUFFER_BIT | GL_COLOR_BUFFER_BIT);
    glDisable(GL_LIGHTING);
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, g_tex[TX_GLOW]);
    glEnable(GL_BLEND);
    glDepthMask(GL_FALSE);
    for (int pass = 0; pass < 2; ++pass) {
        glBlendFunc(GL_SRC_ALPHA, pass == 0 ? GL_ONE_MINUS_SRC_ALPHA : GL_ONE);
        glBegin(GL_QUADS);
        for (size_t i = 0; i < g_parts.size(); ++i) {
            const Particle& q = g_parts[i];
            if (q.add != (pass == 1)) continue;
            float k = q.life / q.maxLife;
            glColor4f(q.r, q.g, q.b, q.a * (q.add ? k : std::min(1.0f, k * 2.0f)));
            const float s = q.size;
            glTexCoord2f(0, 0); glVertex3f(q.p[0] - (R[0] + U[0]) * s, q.p[1] - (R[1] + U[1]) * s, q.p[2] - (R[2] + U[2]) * s);
            glTexCoord2f(1, 0); glVertex3f(q.p[0] + (R[0] - U[0]) * s, q.p[1] + (R[1] - U[1]) * s, q.p[2] + (R[2] - U[2]) * s);
            glTexCoord2f(1, 1); glVertex3f(q.p[0] + (R[0] + U[0]) * s, q.p[1] + (R[1] + U[1]) * s, q.p[2] + (R[2] + U[2]) * s);
            glTexCoord2f(0, 1); glVertex3f(q.p[0] - (R[0] - U[0]) * s, q.p[1] - (R[1] - U[1]) * s, q.p[2] - (R[2] - U[2]) * s);
        }
        glEnd();
    }
    glPopAttrib();
}

// ---- marcas de tiro
// Quando um projetil bate em uma parede, caixa, chao ou teto, fica uma marca (furo com borda lascada e fuligem)
// colada na superficie, no ponto do impacto. Guarda as ultimas MAX_DECALS.
struct Decal { float p[3], n[3], rot, size; };
static const int MAX_DECALS = 400;
static Decal g_decals[MAX_DECALS];
static int g_decalCount = 0, g_decalNext = 0;

// Primeiro ponto em que o segmento a->b entra em uma superficie (caixas, chao, teto) e a normal dela.
static bool segmentHit(const float a[3], const float b[3], float hit[3], float n[3]) {
    float d[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] };
    float best = 1.0f;
    bool found = false;
    n[0] = n[1] = n[2] = 0.0f;
    if (a[1] > 0.0f && b[1] <= 0.0f) {
        best = a[1] / (a[1] - b[1]); found = true; n[0] = 0; n[1] = 1; n[2] = 0;
    }
    if (a[1] < g_ceil && b[1] >= g_ceil) {
        float t = (g_ceil - a[1]) / (b[1] - a[1]);
        if (t < best) { best = t; found = true; n[0] = 0; n[1] = -1; n[2] = 0; }
    }
    for (int i = 0; i < g_numObs; ++i) {
        const Obstacle& o = g_obs[i];
        float lo[3] = { o.x - o.hw, 0.0f, o.z - o.hd }, hi[3] = { o.x + o.hw, o.h, o.z + o.hd };
        float t0 = 0.0f, t1 = best;
        int axis = -1;
        float sgn = 0.0f;
        bool ok = true;
        for (int k = 0; k < 3 && ok; ++k) {
            if (std::fabs(d[k]) < 1e-8f) {
                if (a[k] < lo[k] || a[k] > hi[k]) ok = false;
            } else {
                float ta = (lo[k] - a[k]) / d[k], tb = (hi[k] - a[k]) / d[k];
                if (ta > tb) { float tmp = ta; ta = tb; tb = tmp; }
                if (ta > t0) { t0 = ta; axis = k; sgn = d[k] > 0.0f ? -1.0f : 1.0f; }
                if (tb < t1) t1 = tb;
                if (t0 > t1) ok = false;
            }
        }
        if (ok && axis >= 0 && t0 < best) {
            best = t0; found = true;
            n[0] = n[1] = n[2] = 0.0f;
            n[axis] = sgn;
        }
    }
    if (!found) return false;
    for (int k = 0; k < 3; ++k) hit[k] = a[k] + d[k] * best;
    return true;
}

static void addDecal(const float from[3], const float to[3]) {
    float hit[3], n[3];
    if (!segmentHit(from, to, hit, n)) return;
    Decal& d = g_decals[g_decalNext];
    g_decalNext = (g_decalNext + 1) % MAX_DECALS;
    if (g_decalCount < MAX_DECALS) ++g_decalCount;
    for (int k = 0; k < 3; ++k) { d.p[k] = hit[k]; d.n[k] = n[k]; }
    d.rot = frand() * 6.2831853f;
    d.size = 0.85f + frand() * 0.4f;
    float sp[3] = { hit[0] + n[0] * 0.05f, hit[1] + n[1] * 0.05f, hit[2] + n[2] * 0.05f };
    spawnImpact(sp, n);
}

static void drawDecalDisk(const float c[3], const float u[3], const float v[3], const float n[3],
                          float off, float radius, float rot, float rough, int sides) {
    glBegin(GL_TRIANGLE_FAN);
    glVertex3f(c[0] + n[0] * off, c[1] + n[1] * off, c[2] + n[2] * off);
    for (int i = 0; i <= sides; ++i) {
        float th = 2.0f * PI * (i % sides) / sides;
        float r = radius * (1.0f + rough * std::sin(5.0f * th + rot * 3.0f) + rough * 0.6f * std::sin(9.0f * th + rot));
        float cx = std::cos(th) * r, cy = std::sin(th) * r;
        glVertex3f(c[0] + u[0] * cx + v[0] * cy + n[0] * off,
                   c[1] + u[1] * cx + v[1] * cy + n[1] * off,
                   c[2] + u[2] * cx + v[2] * cy + n[2] * off);
    }
    glEnd();
}

static void drawDecals() {
    if (g_decalCount == 0) return;
    glDisable(GL_LIGHTING);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDepthMask(GL_FALSE);
    for (int i = 0; i < g_decalCount; ++i) {
        const Decal& d = g_decals[i];
        float h[3] = { 0.0f, 1.0f, 0.0f };
        if (std::fabs(d.n[1]) > 0.9f) { h[0] = 1.0f; h[1] = 0.0f; }
        float u[3] = { h[1] * d.n[2] - h[2] * d.n[1], h[2] * d.n[0] - h[0] * d.n[2], h[0] * d.n[1] - h[1] * d.n[0] };
        norm3(u);
        float v[3] = { d.n[1] * u[2] - d.n[2] * u[1], d.n[2] * u[0] - d.n[0] * u[2], d.n[0] * u[1] - d.n[1] * u[0] };
        glColor4f(0.02f, 0.02f, 0.02f, 0.32f);                                   // fuligem ao redor
        drawDecalDisk(d.p, u, v, d.n, 0.010f, 0.30f * d.size, d.rot, 0.12f, 14);
        glColor4f(0.74f, 0.73f, 0.70f, 0.60f);                                   // borda lascada, mais clara
        drawDecalDisk(d.p, u, v, d.n, 0.016f, 0.15f * d.size, d.rot + 1.0f, 0.22f, 12);
        glColor4f(0.0f, 0.0f, 0.0f, 0.97f);                                      // o furo
        drawDecalDisk(d.p, u, v, d.n, 0.022f, 0.075f * d.size, d.rot + 2.0f, 0.12f, 10);
    }
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
    glEnable(GL_LIGHTING);
}

// Sombra de contato sob o personagem: mancha suave, mais forte onde bate luz direta.
static void drawShadow(float x, float y, float z, float strength = 1.0f) {
    const float r = 0.62f * SCALE;
    glDisable(GL_LIGHTING);
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, g_tex[TX_GLOW]);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDepthMask(GL_FALSE);
    glColor4f(0.0f, 0.0f, 0.0f, 0.40f + 0.50f * clampf(strength, 0.0f, 1.0f));
    glBegin(GL_QUADS);
    glTexCoord2f(0, 0); glVertex3f(x - r, y + 0.03f, z - r);
    glTexCoord2f(1, 0); glVertex3f(x + r, y + 0.03f, z - r);
    glTexCoord2f(1, 1); glVertex3f(x + r, y + 0.03f, z + r);
    glTexCoord2f(0, 1); glVertex3f(x - r, y + 0.03f, z + r);
    glEnd();
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
    glDisable(GL_TEXTURE_2D);
    glEnable(GL_LIGHTING);
}

// Personagem de caixas: usado so se os arquivos .mdl nao forem encontrados.
static void drawBoxPlayer(const Player& p) {
    float legSwing = std::sin(p.walkPhase * 2.0f * PI) * 0.8f * p.swing;

    glPushMatrix();
    glTranslatef(p.x, p.y, p.z);
    glRotatef(p.face * 180.0f / PI, 0, 1, 0);
    glScalef(SCALE, SCALE, SCALE);

    drawLimb(-0.16f, 0.8f, 0, 0.22f, 0.8f, 0.26f,  legSwing, 0.20f, 0.25f, 0.55f);
    drawLimb( 0.16f, 0.8f, 0, 0.22f, 0.8f, 0.26f, -legSwing, 0.20f, 0.25f, 0.55f);
    drawLimb(-0.41f, 1.5f, 0, 0.18f, 0.7f, 0.2f, -legSwing, 0.85f, 0.25f, 0.20f);
    drawLimb( 0.41f, 1.5f, 0, 0.18f, 0.7f, 0.2f,  legSwing, 0.85f, 0.25f, 0.20f);
    glPushMatrix();
    glTranslatef(0, 1.15f, 0);
    glColor3f(0.85f, 0.25f, 0.20f);
    drawBox(0.6f, 0.7f, 0.34f);
    glPopMatrix();
    glPushMatrix();
    glTranslatef(0, 1.79f, 0);
    glColor3f(0.95f, 0.78f, 0.62f);
    drawBox(0.38f, 0.38f, 0.38f);
    glColor3f(0.1f, 0.1f, 0.1f);
    glPushMatrix(); glTranslatef(-0.09f, 0.04f, 0.2f); drawBox(0.07f, 0.07f, 0.03f); glPopMatrix();
    glPushMatrix(); glTranslatef( 0.09f, 0.04f, 0.2f); drawBox(0.07f, 0.07f, 0.03f); glPopMatrix();
    glPopMatrix();

    glPopMatrix();
}

// Fuzil feito de caixas, preso ao peito do personagem (chamar depois de poseModel).
static void drawWeapon(const Model& m, float flash) {
    ArmGeom ag;
    armGeom(m, ag);

    glPushMatrix();
    glScalef(m.scale, m.scale, m.scale);
    glTranslatef(0.0f, -m.minY, 0.0f);
    glMultMatrixf(m.skinM[(size_t)m.spine3].m);
    glTranslatef(ag.C[0], ag.C[1], ag.C[2]);
    glScalef(ag.A, ag.A, ag.A);                         // a partir daqui, 1 unidade = alcance do braco
    glTranslatef(m.wPos[0], m.wPos[1], m.wPos[2]);                     // mesma conta de weaponPoint
    glRotatef(m.wYaw * 180.0f / PI, 0, 1, 0);
    glTranslatef(0.0f, 0.0f, -m.wKick);
    glScalef(WEAPON_SCALE, WEAPON_SCALE, WEAPON_SCALE);
    glTranslatef(WEAPON_PIVOT[0], WEAPON_PIVOT[1], WEAPON_PIVOT[2]);
    glRotatef(-m.wTilt * 180.0f / PI, 1, 0, 0);
    glRotatef(m.wRoll * 180.0f / PI, 0, 0, 1);
    glTranslatef(-WEAPON_PIVOT[0], -WEAPON_PIVOT[1], -WEAPON_PIVOT[2]);

    struct Piece { float y, z, sx, sy, sz, shade; bool mag; };
    static const Piece pieces[] = {
        {  0.000f, 0.150f, 0.050f, 0.090f, 0.260f, 0.30f, false },   // coronha
        { -0.002f, 0.020f, 0.056f, 0.110f, 0.030f, 0.10f, false },   // soleira
        {  0.000f, 0.470f, 0.070f, 0.100f, 0.380f, 0.16f, false },   // corpo
        { -0.095f, 0.380f, 0.045f, 0.100f, 0.050f, 0.12f, false },   // empunhadura
        { -0.125f, 0.550f, 0.050f, 0.150f, 0.070f, 0.10f, true  },   // carregador
        { -0.005f, 0.780f, 0.060f, 0.075f, 0.240f, 0.20f, false },   // guarda-mao
        {  0.058f, 0.600f, 0.030f, 0.018f, 0.520f, 0.11f, false },   // trilho
        {  0.000f, 1.060f, 0.025f, 0.025f, 0.320f, 0.10f, false },   // cano
        {  0.000f, 1.235f, 0.036f, 0.036f, 0.070f, 0.07f, false },   // quebra-chamas
        {  0.098f, 0.500f, 0.036f, 0.050f, 0.110f, 0.09f, false },   // mira optica
        {  0.065f, 0.880f, 0.020f, 0.045f, 0.030f, 0.10f, false },   // mira dianteira
    };
    for (size_t i = 0; i < sizeof(pieces) / sizeof(pieces[0]); ++i) {
        const Piece& pc = pieces[i];
        if (pc.mag && m.magHidden) continue;
        glPushMatrix();
        glTranslatef(0.0f, pc.y - (pc.mag ? m.magDrop : 0.0f), pc.z);
        glColor3f(pc.shade * 1.7f, pc.shade * 1.7f, pc.shade * 1.9f);   // mais claro que a roupa preta, para se destacar
        drawBox(pc.sx * 1.4f, pc.sy * 1.3f, pc.sz);
        glPopMatrix();
    }
    glPushAttrib(GL_ENABLE_BIT | GL_DEPTH_BUFFER_BIT | GL_COLOR_BUFFER_BIT);
    glDisable(GL_LIGHTING);
    glColor3f(1.0f, 0.15f, 0.1f);                       // lente da mira
    glPushMatrix();
    glTranslatef(0.0f, 0.098f, 0.558f);
    drawBox(0.030f, 0.036f, 0.004f);
    glPopMatrix();
    if (flash > 0.0f) {                                 // clarao na boca do cano: estrela de frente + dois jatos cruzados
        float k = clampf(flash / 0.06f, 0.0f, 1.0f), a = 0.35f + 0.65f * k;
        float r0 = (0.20f + 0.10f * k), L = 0.55f + 0.25f * k, w = 0.10f;
        const float z0 = MUZZLE_Z + 0.02f;
        glEnable(GL_TEXTURE_2D);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE);
        glDepthMask(GL_FALSE);
        glColor4f(1.0f, 0.86f, 0.55f, a);
        glBindTexture(GL_TEXTURE_2D, textureId(2));
        glPushMatrix();
        glTranslatef(0.0f, 0.0f, z0 + 0.03f);
        glRotatef(g_time * 2311.0f, 0, 0, 1);            // cada tiro com a estrela em outro angulo
        glBegin(GL_QUADS);
        glTexCoord2f(0, 0); glVertex3f(-r0, -r0, 0); glTexCoord2f(1, 0); glVertex3f(r0, -r0, 0);
        glTexCoord2f(1, 1); glVertex3f(r0, r0, 0);   glTexCoord2f(0, 1); glVertex3f(-r0, r0, 0);
        glEnd();
        glPopMatrix();
        glBindTexture(GL_TEXTURE_2D, textureId(3));
        glBegin(GL_QUADS);
        glTexCoord2f(0, 0); glVertex3f(-w, 0, z0 - 0.05f); glTexCoord2f(1, 0); glVertex3f(w, 0, z0 - 0.05f);
        glTexCoord2f(1, 1); glVertex3f(w, 0, z0 + L);      glTexCoord2f(0, 1); glVertex3f(-w, 0, z0 + L);
        glTexCoord2f(0, 0); glVertex3f(0, -w, z0 - 0.05f); glTexCoord2f(1, 0); glVertex3f(0, w, z0 - 0.05f);
        glTexCoord2f(1, 1); glVertex3f(0, w, z0 + L);      glTexCoord2f(0, 1); glVertex3f(0, -w, z0 + L);
        glEnd();
    }
    glPopAttrib();
    glPopMatrix();
}

// Desenha um personagem (o jogador ou o bot). armed: 0..1 pose com arma; fall: 0..1 caindo para tras (eliminado).
static void drawCharacter(const Player& p, int modelIdx, const Look& look, float armed, float flash, float hurt, float fall,
                          const PoseFx& fx) {
    Model& m = g_models[modelIdx];
    if (!m.loaded) { drawBoxPlayer(p); return; }

    poseModel(m, p.walkPhase, p.speed, g_time, armed, &fx);

    glPushMatrix();
    glTranslatef(p.x, p.y, p.z);
    glRotatef(p.face * 180.0f / PI, 0, 1, 0);
    if (fall > 0.0f) {                                  // cai de costas, girando nos pes, e fica deitado sobre o chao
        glTranslatef(0.0f, 0.32f * smooth01(fall), 0.0f);
        glRotatef(-90.0f * smooth01(fall), 1, 0, 0);
    }
    drawModel(m, 1.0f, look, hurt);
    if (armed > 0.35f) drawWeapon(m, flash);
    glPopMatrix();
}

// Quanto o tronco do jogador inclina para mirar: segue a camera, mas so quando o corpo esta virado para onde ela aponta.
static float playerAimPitch() {
    float align = std::cos(std::atan2(-std::sin(g_camYaw), -std::cos(g_camYaw)) - g_player.face);
    return g_camPitch * clampf(align, 0.0f, 1.0f);
}

static void drawPlayer(const Player& p) {
    float hurt = g_hurtTimer > 0.0f ? g_hurtTimer / HURT_TIME : 0.0f;
    PoseFx fx;
    fx.aimPitch = playerAimPitch();
    fx.recoil = g_recoil;
    fx.fire = smooth01(g_fireBlend);
    fx.reload = g_reloadTimer > 0.0f && g_reloadTotal > 0.0f ? 1.0f - g_reloadTimer / g_reloadTotal : -1.0f;
    fx.air = p.air;
    fx.vy = clampf(p.vy / (9.0f * SCALE), -1.0f, 1.0f);
    fx.land = p.land;
    Look lk = g_look[g_choice];
    if (g_net != NET_NONE && g_mode == MODE_TEAMS) { lk.outfit = true; lk.team = g_myTeam == 0 ? 2 : 1; }   // colete da equipe
    drawCharacter(p, g_choice, lk, smooth01(g_armBlend), g_flash, hurt, g_playerFall, fx);
}

static void setupView(float ex, float ey, float ez, float tx, float ty, float tz) {
    float aspect = (float)g_width / (float)(g_height > 0 ? g_height : 1);
    float nearP = 0.1f, farP = 700.0f;                  // longe o bastante para os morros do deserto
    float top = nearP * std::tan(60.0f * PI / 180.0f * 0.5f);
    float right = top * aspect;

    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glFrustum(-right, right, -top, top, nearP, farP);

    // lookAt manual (olhando de (ex,ey,ez) para (tx,ty,tz), up = +Y)
    float fx = tx - ex, fy = ty - ey, fz = tz - ez;
    float fl = std::sqrt(fx * fx + fy * fy + fz * fz);
    fx /= fl; fy /= fl; fz /= fl;
    float sx = -fz, sy = 0.0f, sz = fx;                                        // f x up
    float sl = std::sqrt(sx * sx + sy * sy + sz * sz);
    sx /= sl; sy /= sl; sz /= sl;
    float ux = sy * fz - sz * fy, uy = sz * fx - sx * fz, uz = sx * fy - sy * fx;  // s x f

    float m[16] = {
        sx, ux, -fx, 0,
        sy, uy, -fy, 0,
        sz, uz, -fz, 0,
        0,  0,  0,   1
    };
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glMultMatrixf(m);
    glTranslatef(-ex, -ey, -ez);
}

// Camera de terceira pessoa. Com a arma na mao ela passa para cima do ombro: o personagem sai do centro da tela, que
// fica livre para a mira. Paredes e caixas no caminho a aproximam do jogador em vez de deixa-la atravessar; com uma
// parede logo atras, ela tambem sobe e olha por cima da cabeca dele, sem mudar a direcao da visada: a mira continua
// no centro da tela apontando para onde o jogador escolheu. updateCamera mexe nesses valores aos poucos, sem saltos.
static const float SHOULDER_OFFSET = 1.3f;
static const float CAM_HEIGHT = 1.4f * SCALE;   // altura do ponto para onde a camera olha, acima dos pes
static const float CAM_MARGIN = 0.3f;           // folga entre a camera e a superficie mais proxima
static const float CAM_MIN_DIST = 0.2f;
static const float CAM_MAX_LIFT = 1.6f;
static float g_shoulder = 0.0f;                 // deslocamento lateral atual
static float g_camLift = 0.0f;                  // quanto a camera subiu por causa de uma parede atras
static float g_camDist = CAM_DIST;              // distancia atual ate o ponto olhado

static float aimRayHit(const float o[3], const float d[3], float tMin, float tMax);

static void camDir(float pitch, float e[3]) {   // do ponto olhado em direcao a camera
    e[0] = std::cos(pitch) * std::sin(g_camYaw);
    e[1] = std::sin(pitch);
    e[2] = std::cos(pitch) * std::cos(g_camYaw);
}

static void camTarget(const Player& p, float t[3]) {
    t[0] = p.x + g_shoulder * std::cos(g_camYaw);
    t[1] = p.y + CAM_HEIGHT + g_camLift;
    t[2] = p.z - g_shoulder * std::sin(g_camYaw);
}

static void updateCamera(const Player& p, float dt) {
    // ombro: encurta na hora se houver parede ao lado (senao a camera entraria nela), volta devagar
    float chest[3] = { p.x, p.y + CAM_HEIGHT, p.z };
    float right[3] = { std::cos(g_camYaw), 0.0f, -std::sin(g_camYaw) };
    float want = SHOULDER_OFFSET * smooth01(g_armBlend);
    want = std::min(want, std::max(aimRayHit(chest, right, 0.0f, want + CAM_MARGIN) - CAM_MARGIN, 0.0f));
    g_shoulder = want < g_shoulder ? want : g_shoulder + (want - g_shoulder) * (1.0f - std::exp(-6.0f * dt));

    // quanto mais perto a parede atras, mais a camera sobe (o personagem nao tapa a tela)
    float t0[3] = { p.x + g_shoulder * right[0], chest[1], p.z + g_shoulder * right[2] }, e[3];
    camDir(g_camPitch, e);
    float free0 = clampf(aimRayHit(t0, e, 0.0f, CAM_DIST + CAM_MARGIN) - CAM_MARGIN, CAM_MIN_DIST, CAM_DIST);
    float liftWant = (1.0f - free0 / CAM_DIST) * (1.0f - free0 / CAM_DIST) * CAM_MAX_LIFT;
    liftWant = std::min(liftWant, g_ceil - 1.0f - t0[1]);
    g_camLift += (liftWant - g_camLift) * (1.0f - std::exp(-8.0f * dt));

    float t[3];
    camTarget(p, t);
    float free1 = clampf(aimRayHit(t, e, 0.0f, CAM_DIST + CAM_MARGIN) - CAM_MARGIN, CAM_MIN_DIST, CAM_DIST);
    g_camDist = free1 < g_camDist ? free1 : g_camDist + (free1 - g_camDist) * (1.0f - std::exp(-5.0f * dt));
}

static void orbitCamera(const Player& p, float t[3], float eye[3]) {
    float e[3];
    camTarget(p, t);
    camDir(g_camPitch, e);
    for (int i = 0; i < 3; ++i) eye[i] = t[i] + e[i] * g_camDist;
}

static void setupOrbitCamera(const Player& p) {
    float t[3], eye[3];
    orbitCamera(p, t, eye);
    setupView(eye[0], eye[1], eye[2], t[0], t[1], t[2]);
}

// ---------------------------------------------------------------- texto
// As letras sao desenhadas uma unica vez pelo Windows, com bordas suavizadas, em uma textura (16 x 14 letras). O jogo
// desenha cada letra como um retangulo dessa textura: o texto fica liso e pode ter sombra e transparencia.

struct Font {
    GLuint tex;
    int cellW, cellH, pad, ascent, height, texW, texH;
    float adv[224];             // avanco de cada letra (caracteres 32..255, com acentos)
};

static Font makeFont(HDC, int px, int weight) {
    Font f;
    memset(&f, 0, sizeof(f));
    HDC dc = CreateCompatibleDC(NULL);
    HFONT hf = CreateFontA(-px, 0, 0, 0, weight, FALSE, FALSE, FALSE, ANSI_CHARSET, OUT_TT_PRECIS,
                           CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE, "Segoe UI");
    HGDIOBJ oldFont = SelectObject(dc, hf);
    TEXTMETRICA tm;
    GetTextMetricsA(dc, &tm);
    int maxAdv = 1;
    for (int c = 0; c < 224; ++c) {
        INT w = px / 2;
        GetCharWidth32A(dc, (UINT)(c + 32), (UINT)(c + 32), &w);
        f.adv[c] = (float)w;
        if (w > maxAdv) maxAdv = w;
    }
    f.pad = 4;
    f.ascent = tm.tmAscent;
    f.height = tm.tmHeight;
    f.cellW = maxAdv + 2 * f.pad;
    f.cellH = tm.tmHeight + 2 * f.pad;
    f.texW = 64; while (f.texW < 16 * f.cellW) f.texW *= 2;
    f.texH = 64; while (f.texH < 14 * f.cellH) f.texH *= 2;

    BITMAPINFO bi;
    memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = f.texW;
    bi.bmiHeader.biHeight = -f.texH;        // de cima para baixo
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = NULL;
    HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    std::vector<unsigned char> alpha((size_t)f.texW * f.texH, 0);
    if (bmp && bits) {
        HGDIOBJ oldBmp = SelectObject(dc, bmp);
        memset(bits, 0, (size_t)f.texW * f.texH * 4);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(255, 255, 255));
        for (int c = 0; c < 224; ++c) {
            char ch = (char)(c + 32);
            TextOutA(dc, (c % 16) * f.cellW + f.pad, (c / 16) * f.cellH + f.pad, &ch, 1);
        }
        GdiFlush();
        const unsigned char* src = (const unsigned char*)bits;
        for (int y = 0; y < f.texH; ++y)                    // a textura cresce para cima: inverte as linhas
            for (int x = 0; x < f.texW; ++x) {
                const unsigned char* p = src + ((size_t)y * f.texW + x) * 4;
                alpha[(size_t)(f.texH - 1 - y) * f.texW + x] = std::max(p[0], std::max(p[1], p[2]));
            }
        SelectObject(dc, oldBmp);
        DeleteObject(bmp);
    }
    SelectObject(dc, oldFont);
    DeleteObject(hf);
    DeleteDC(dc);

    glGenTextures(1, &f.tex);
    glBindTexture(GL_TEXTURE_2D, f.tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_ALPHA, f.texW, f.texH, 0, GL_ALPHA, GL_UNSIGNED_BYTE, alpha.data());
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glBindTexture(GL_TEXTURE_2D, 0);
    return f;
}

static float textWidth(const Font& f, const char* s) {
    float w = 0.0f;
    for (; *s; ++s) { unsigned char c = (unsigned char)*s; if (c >= 32) w += f.adv[c - 32]; }
    return w;
}

// Emite os retangulos das letras. (ox, oy, oz) = inicio da linha de base; (rx..) e (ux..) = direcoes "direita" e
// "cima" ja no tamanho de um pixel. Chamar entre glBegin(GL_QUADS) e glEnd().
static void emitGlyphs(const Font& f, const char* s, const float o[3], const float r[3], const float u[3], float dx, float dy) {
    float pen = dx;
    float y0 = dy - (float)(f.height - f.ascent) - (float)f.pad, y1 = y0 + (float)f.cellH;
    for (; *s; ++s) {
        unsigned char c = (unsigned char)*s;
        if (c < 32) continue;
        int i = c - 32;
        float x0 = pen - (float)f.pad, x1 = x0 + (float)f.cellW;
        float u0 = (float)((i % 16) * f.cellW) / f.texW, u1 = (float)((i % 16 + 1) * f.cellW) / f.texW;
        float v1 = 1.0f - (float)((i / 16) * f.cellH) / f.texH, v0 = 1.0f - (float)((i / 16 + 1) * f.cellH) / f.texH;
        glTexCoord2f(u0, v0); glVertex3f(o[0] + r[0] * x0 + u[0] * y0, o[1] + r[1] * x0 + u[1] * y0, o[2] + r[2] * x0 + u[2] * y0);
        glTexCoord2f(u1, v0); glVertex3f(o[0] + r[0] * x1 + u[0] * y0, o[1] + r[1] * x1 + u[1] * y0, o[2] + r[2] * x1 + u[2] * y0);
        glTexCoord2f(u1, v1); glVertex3f(o[0] + r[0] * x1 + u[0] * y1, o[1] + r[1] * x1 + u[1] * y1, o[2] + r[2] * x1 + u[2] * y1);
        glTexCoord2f(u0, v1); glVertex3f(o[0] + r[0] * x0 + u[0] * y1, o[1] + r[1] * x0 + u[1] * y1, o[2] + r[2] * x0 + u[2] * y1);
        pen += f.adv[i];
    }
}

static void begin2D() {
    glPushAttrib(GL_ENABLE_BIT | GL_TRANSFORM_BIT);
    glDisable(GL_LIGHTING);
    glDisable(GL_FOG);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_TEXTURE_2D);
    glMatrixMode(GL_PROJECTION);
    glPushMatrix();
    glLoadIdentity();
    glOrtho(0, g_width, 0, g_height, -1, 1);
    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glLoadIdentity();
}

static void end2D() {
    glMatrixMode(GL_PROJECTION);
    glPopMatrix();
    glMatrixMode(GL_MODELVIEW);
    glPopMatrix();
    glPopAttrib();
}

// Texto centrado em x (coordenadas de tela, y = linha de base, de baixo para cima), com sombra. a = opacidade.
static void drawText(HDC, const Font& f, const char* s, float cx, float y, float r, float g, float b, float a = 1.0f) {
    const float o[3] = { std::floor(cx - textWidth(f, s) * 0.5f + 0.5f), std::floor(y + 0.5f), 0.0f };
    const float X[3] = { 1, 0, 0 }, Y[3] = { 0, 1, 0 };
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, f.tex);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glBegin(GL_QUADS);
    glColor4f(0.0f, 0.0f, 0.0f, 0.55f * a);
    emitGlyphs(f, s, o, X, Y, 2.0f, -2.0f);
    emitGlyphs(f, s, o, X, Y, 1.0f, -1.0f);
    if (g_worldMap == MAP_BRANCO) {                     // fundo branco: contorno escuro em volta das letras
        glColor4f(0.0f, 0.0f, 0.0f, 0.75f * a);
        static const float off[8][2] = { { -1.5f, 0 }, { 1.5f, 0 }, { 0, -1.5f }, { 0, 1.5f }, { -1, -1 }, { 1, -1 }, { -1, 1 }, { 1, 1 } };
        for (int k = 0; k < 8; ++k) emitGlyphs(f, s, o, X, Y, off[k][0], off[k][1]);
    }
    glColor4f(r, g, b, a);
    emitGlyphs(f, s, o, X, Y, 0.0f, 0.0f);
    glEnd();
    glDisable(GL_BLEND);
    glDisable(GL_TEXTURE_2D);
}

// Texto centrado sobre um ponto do mundo (ex.: nome acima da cabeca), sempre de frente para a camera.
// Tamanho fixo em pixels; some atras de objetos.
static void drawText3D(HDC, const Font& f, const char* s, float x, float y, float z, float r, float g, float b) {
    float mv[16];
    glGetFloatv(GL_MODELVIEW_MATRIX, mv);
    float depth = -(mv[2] * x + mv[6] * y + mv[10] * z + mv[14]);
    if (depth < 0.3f || g_height <= 0) return;
    float fade = 1.0f;                                  // sala branca: o nome some na nevoa junto com o personagem
    if (g_worldMap == MAP_BRANCO) { fade = clampf((75.0f - depth) / 35.0f, 0.0f, 1.0f); if (fade <= 0.0f) return; }
    float px = 2.0f * depth * std::tan(30.0f * PI / 180.0f) / (float)g_height;      // tamanho de um pixel nessa distancia
    const float R[3] = { mv[0] * px, mv[4] * px, mv[8] * px }, U[3] = { mv[1] * px, mv[5] * px, mv[9] * px };
    const float o[3] = { x, y, z };
    float half = -textWidth(f, s) * 0.5f;

    glPushAttrib(GL_ENABLE_BIT | GL_DEPTH_BUFFER_BIT);
    glDisable(GL_LIGHTING);
    glDisable(GL_FOG);
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, f.tex);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDepthMask(GL_FALSE);
    glBegin(GL_QUADS);
    glColor4f(0.0f, 0.0f, 0.0f, 0.6f * fade);
    emitGlyphs(f, s, o, R, U, half + 2.0f, -2.0f);
    emitGlyphs(f, s, o, R, U, half + 1.0f, -1.0f);
    if (g_worldMap == MAP_BRANCO) {
        emitGlyphs(f, s, o, R, U, half - 1.5f, 0.0f);
        emitGlyphs(f, s, o, R, U, half + 1.5f, 0.0f);
        emitGlyphs(f, s, o, R, U, half, 1.5f);
        emitGlyphs(f, s, o, R, U, half, -1.5f);
    }
    glColor4f(r, g, b, fade);
    emitGlyphs(f, s, o, R, U, half, 0.0f);
    glEnd();
    glPopAttrib();
}

// ---------------------------------------------------------------- logica

// Altura do chao sob (x, z): 0 ou o topo de uma caixa que o jogador consegue pisar.
static float groundHeightAt(float x, float z, float feetY) {
    float best = 0.0f;
    for (int i = 0; i < g_numObs; ++i) {
        const Obstacle& o = g_obs[i];
        if (std::fabs(x - o.x) <= o.hw && std::fabs(z - o.z) <= o.hd &&
            feetY >= o.h - STEP_HEIGHT && o.h > best)
            best = o.h;
    }
    return best;
}

// Empurra o jogador para fora das caixas mais altas que seus pes.
static void resolveCollisions(Player& p) {
    for (int i = 0; i < g_numObs; ++i) {
        const Obstacle& o = g_obs[i];
        if (p.y >= o.h - STEP_HEIGHT) continue;   // esta por cima

        float cx = clampf(p.x, o.x - o.hw, o.x + o.hw);
        float cz = clampf(p.z, o.z - o.hd, o.z + o.hd);
        float dx = p.x - cx, dz = p.z - cz;
        float d2 = dx * dx + dz * dz;
        if (d2 >= PLAYER_RADIUS * PLAYER_RADIUS) continue;

        if (d2 > 1e-8f) {
            float d = std::sqrt(d2);
            p.x = cx + dx / d * PLAYER_RADIUS;
            p.z = cz + dz / d * PLAYER_RADIUS;
        } else {
            // centro dentro da caixa: sai pelo lado mais proximo
            float l = p.x - (o.x - o.hw), r = (o.x + o.hw) - p.x;
            float t = p.z - (o.z - o.hd), b = (o.z + o.hd) - p.z;
            float m = l;
            if (r < m) m = r;
            if (t < m) m = t;
            if (b < m) m = b;
            if (m == l)      p.x = o.x - o.hw - PLAYER_RADIUS;
            else if (m == r) p.x = o.x + o.hw + PLAYER_RADIUS;
            else if (m == t) p.z = o.z - o.hd - PLAYER_RADIUS;
            else             p.z = o.z + o.hd + PLAYER_RADIUS;
        }
    }
    p.x = clampf(p.x, -ROOM_X + PLAYER_RADIUS, ROOM_X - PLAYER_RADIUS);     // ninguem sai da arena, nem por cima da muralha
    p.z = clampf(p.z, -ROOM_Z + PLAYER_RADIUS, ROOM_Z - PLAYER_RADIUS);
}

static float angleDiff(float target, float cur) {      // diferenca entre angulos, entre -PI e PI
    float d = std::fmod(target - cur + PI, 2.0f * PI); // fmod (e nao um laco): um valor invalido nunca trava o jogo
    if (d < 0.0f) d += 2.0f * PI;
    return d - PI;
}

// Movimento horizontal de um personagem (jogador ou bot): acelera e freia suavemente, vira o corpo, avanca o ciclo
// de passos e empurra para fora das caixas. (dx, dz) e a direcao desejada (unitaria) quando moving = true.
static void moveActor(Player& p, float dx, float dz, bool moving, float maxSpeed, float dt, bool turnToVelocity) {
    float k = 1.0f - std::exp(-(moving ? 9.0f : 11.0f) * dt);
    p.vx += ((moving ? dx * maxSpeed : 0.0f) - p.vx) * k;
    p.vz += ((moving ? dz * maxSpeed : 0.0f) - p.vz) * k;
    p.x += p.vx * dt;
    p.z += p.vz * dt;

    float hs = std::sqrt(p.vx * p.vx + p.vz * p.vz);
    p.speed = hs / SCALE;
    if (turnToVelocity && hs > 0.25f * SCALE)
        p.face += angleDiff(std::atan2(p.vx, p.vz), p.face) * (1.0f - std::exp(-10.0f * dt));
    p.walkPhase += gaitCadence(p.speed) * dt;
    p.walkPhase -= std::floor(p.walkPhase);
    p.swing = clampf(p.speed / 1.5f, 0.0f, 1.0f);
    resolveCollisions(p);
}

// ---- bot: um adversario controlado pelo computador, com as mesmas regras do jogador (vida, municao, recarga)
static const float BOT_AIM_ERROR = 0.11f;   // dificuldade: quanto maior, mais o bot erra (0 = nunca erra parado)
enum BotState { BS_CHASE, BS_ATTACK, BS_MOVE, BS_COVER };
struct Bot {
    Player b;
    int   model = 1;                // indice em g_models (sempre o genero oposto ao do jogador)
    Look  look = { 0, 0, false, true, 1 };
    bool  active = false;
    int   hp = MAX_HP, ammo = MAG_SIZE;
    float reloadTimer = 0, reloadTotal = RELOAD_TIME, fireCd = 0, flash = 0, hurt = 0, invuln = 0;
    bool  dead = false;
    float deadTimer = 0, fall = 0;
    BotState state = BS_CHASE;
    float stateT = 0, burstWait = 0, repath = 0, noSightT = 0;
    int   burstLeft = 0;
    float lastX = 0, lastZ = 0;    // onde o bot acha que o jogador esta: so atualiza quando ve, ouve um tiro ou e atingido
    float alertT = 0;              // > 0: em alerta, percebe o jogador em qualquer direcao
    // Partida em rede: cada outro jogador da sala usa esta mesma estrutura, sem IA -- a posicao chega pela rede.
    bool  remote = false;
    bool  present = false;         // esta na sala (active: esta na partida, no mapa)
    int   team = 0, kills = 0, deaths = 0;
    std::string name = "Bot";
    float arm = 1.0f;              // pose da arma (0 = mao nua)
    float shotHold = 0, fireBlend = 0;                    // como g_shotHold / g_fireBlend
    float aimPitch = 0, recoil = 0, reloadProg = -1.0f;   // mira vertical, coice e progresso da recarga (rede)
    float netX = 0, netY = 0, netZ = 0, netVx = 0, netVz = 0, netFace = 0, netSpeed = 0, netAge = 0;   // ultimo estado recebido
};
static Bot g_bot;
static Bot g_rp[MAX_PLAYERS];      // os outros jogadores da sala, pelo numero de cada um (o meu fica sem uso)

static void netSendHit(int id);
static void netSendShot(const float o[3], const float d[3]);

// Na rede: o jogador id e inimigo? (todos contra todos: todo mundo; equipes: so a outra equipe)
static bool netFoe(int id) { return g_mode == MODE_FFA || g_rp[id].team != g_myTeam; }

// O tiro do jogador faz barulho e denuncia a posicao dele.
static void botHearsPlayer() {
    g_bot.lastX = g_player.x;
    g_bot.lastZ = g_player.z;
    g_bot.alertT = 2.0f;
}

static void botDamage(int hearts) {
    Bot& bt = g_bot;
    if (!bt.active || bt.dead) return;
    if (bt.invuln > 0.0f) return;
    bt.hp = bt.hp - hearts < 0 ? 0 : bt.hp - hearts;
    bt.invuln = INVULN_TIME;
    bt.hurt = 1.0f;
    g_hitMarker = 0.18f;
    botHearsPlayer();
    if (bt.hp == 0) { bt.dead = true; bt.deadTimer = RESPAWN_TIME; ++g_scorePlayer; }
}

// Acertou outro jogador da rede: quem atira decide o acerto e avisa; o atingido tira a propria vida.
static void remoteDamage(int id) {
    if (g_phase != PH_PLAY) return;
    netSendHit(id);
    g_hitMarker = 0.18f;
    g_rp[id].hurt = 1.0f;
}

// ---- tiros
// Projeteis sao cubinhos que voam em linha reta para onde a camera mira e somem ao bater em algo,
// ao sair do campo de visao ou depois de um alcance maximo.
struct Projectile { float p[3], v[3], dist; int owner; };   // owner: 0 = jogador, 1 = bot, 2 + n = jogador n da rede
static std::vector<Projectile> g_proj;
static const float BULLET_SPEED = 55.0f;
static const float BULLET_SIZE = 0.12f;
static const float BULLET_RANGE = 200.0f;      // maior que a diagonal da sala: na pratica o tiro sempre bate em algo
static const float FIRE_INTERVAL = 0.13f;

// Camera de terceira pessoa: posicao do olho e base (frente, direita, cima) -- mesma conta do setupView.
static void cameraRig(const Player& p, float eye[3], float f[3], float s[3], float u[3]) {
    float t[3];
    orbitCamera(p, t, eye);
    for (int i = 0; i < 3; ++i) f[i] = t[i] - eye[i];
    norm3(f);
    s[0] = -f[2]; s[1] = 0.0f; s[2] = f[0];
    norm3(s);
    u[0] = s[1] * f[2] - s[2] * f[1];
    u[1] = s[2] * f[0] - s[0] * f[2];
    u[2] = s[0] * f[1] - s[1] * f[0];
}

// Boca do cano no mundo (usa a ultima pose desenhada).
static void muzzleWorld(const Player& p, int modelIdx, float out[3]) {
    const Model& m = g_models[modelIdx];
    float lx, ly, lz;
    if (m.loaded) {
        ArmGeom ag;
        armGeom(m, ag);
        const float mz[3] = { 0.0f, 0.0f, MUZZLE_Z };
        float wl[3];
        weaponPoint(m, mz, wl);
        float pt[3] = { ag.C[0] + ag.A * wl[0], ag.C[1] + ag.A * wl[1], ag.C[2] + ag.A * wl[2] };
        const float* M = m.skinM[(size_t)m.spine3].m;
        float q[3];
        for (int c = 0; c < 3; ++c) q[c] = M[c] * pt[0] + M[4 + c] * pt[1] + M[8 + c] * pt[2] + M[12 + c];
        lx = q[0] * m.scale;
        ly = (q[1] - m.minY) * m.scale;
        lz = q[2] * m.scale;
    } else {
        lx = 0.0f; ly = 1.3f * SCALE; lz = 0.9f * SCALE;
    }
    float cf = std::cos(p.face), sf = std::sin(p.face);
    out[0] = p.x + lx * cf + lz * sf;
    out[1] = p.y + ly;
    out[2] = p.z - lx * sf + lz * cf;
}

// Distancia ao longo do raio (o, d) ate o primeiro objeto/chao alem de tMin; tMax se nao achar nada.
static float aimRayHit(const float o[3], const float d[3], float tMin, float tMax) {
    float best = tMax;
    if (d[1] < -1e-4f) {                                // chao
        float t = -o[1] / d[1];
        if (t > tMin && t < best) best = t;
    }
    if (d[1] > 1e-4f) {                                 // teto
        float t = (g_ceil - o[1]) / d[1];
        if (t > tMin && t < best) best = t;
    }
    for (int i = 0; i < g_numObs; ++i) {
        const Obstacle& b = g_obs[i];
        float lo[3] = { b.x - b.hw, 0.0f, b.z - b.hd }, hi[3] = { b.x + b.hw, b.h, b.z + b.hd };
        float t0 = tMin, t1 = best;
        bool ok = true;
        for (int a = 0; a < 3 && ok; ++a) {
            if (std::fabs(d[a]) < 1e-6f) {
                if (o[a] < lo[a] || o[a] > hi[a]) ok = false;
            } else {
                float ta = (lo[a] - o[a]) / d[a], tb = (hi[a] - o[a]) / d[a];
                if (ta > tb) { float tmp = ta; ta = tb; tb = tmp; }
                if (ta > t0) t0 = ta;
                if (tb < t1) t1 = tb;
                if (t0 > t1) ok = false;
            }
        }
        if (ok && t0 < best) best = t0;
    }
    return best;
}

static void spawnProjectile(int owner, const Player& shooter, const float muzzle[3], const float dir[3]) {
    // encostado em uma parede, o cano entra nela: o tiro acerta a parede ali mesmo em vez de nascer do outro lado
    float body[3] = { shooter.x, muzzle[1], shooter.z }, hit[3], n[3];
    if (segmentHit(body, muzzle, hit, n)) { addDecal(body, muzzle); return; }
    Projectile b;
    for (int i = 0; i < 3; ++i) { b.p[i] = muzzle[i]; b.v[i] = dir[i] * BULLET_SPEED; }
    b.dist = 0.0f;
    b.owner = owner;
    if (g_proj.size() < 256) g_proj.push_back(b);
}

// Hitbox de um personagem: cilindro vertical de raio HIT_RADIUS e altura PLAYER_HEIGHT sobre os pes.
static const float HIT_RADIUS = 0.55f * SCALE * 0.8f;

static bool insideCharacter(const float p[3], const Player& c) {
    float dx = p[0] - c.x, dz = p[2] - c.z;
    return dx * dx + dz * dz < HIT_RADIUS * HIT_RADIUS && p[1] > c.y && p[1] < c.y + PLAYER_HEIGHT;
}

// Algum jogador da rede esta em p? Ignora o dono do tiro e os companheiros de equipe dele. Devolve o numero ou -1.
static int remoteAt(const float p[3], int ownerId, int ownerTeam) {
    for (int i = 0; i < MAX_PLAYERS; ++i) {
        const Bot& r = g_rp[i];
        if (i == ownerId || i == g_myId || !r.present || !r.active || r.dead) continue;
        if (g_mode == MODE_TEAMS && r.team == ownerTeam) continue;
        if (insideCharacter(p, r.b)) return i;
    }
    return -1;
}

// Distancia ao longo do raio (o, d) ate o cilindro do personagem, ou -1 se nao acerta (entre tMin e tMax).
static float rayCharacterHit(const float o[3], const float d[3], const Player& c, float tMin, float tMax) {
    float ox = o[0] - c.x, oz = o[2] - c.z;
    float a = d[0] * d[0] + d[2] * d[2];
    if (a < 1e-8f) return -1.0f;
    float b = ox * d[0] + oz * d[2];
    float cc = ox * ox + oz * oz - HIT_RADIUS * HIT_RADIUS;
    float disc = b * b - a * cc;
    if (disc < 0.0f) return -1.0f;
    float t = (-b - std::sqrt(disc)) / a;
    if (t < tMin || t > tMax) return -1.0f;
    float y = o[1] + d[1] * t;
    return (y > c.y && y < c.y + PLAYER_HEIGHT) ? t : -1.0f;
}

// Ponto mirado: o que a linha de visada (o centro da tela) encontra primeiro: parede, caixa, chao, teto ou o bot.
static void aimPoint(const Player& p, float out[3], float dir[3]) {
    float t[3], e[3];
    camTarget(p, t);
    camDir(g_camPitch, e);
    for (int i = 0; i < 3; ++i) dir[i] = -e[i];
    float d = aimRayHit(t, dir, 0.0f, 195.0f);
    if (g_bot.active && !g_bot.dead) {
        float tb = rayCharacterHit(t, dir, g_bot.b, 0.0f, d);
        if (tb > 0.0f) d = tb;
    }
    for (int i = 0; i < MAX_PLAYERS; ++i) {             // na rede: os inimigos (o tiro atravessa os companheiros)
        const Bot& r = g_rp[i];
        if (i == g_myId || !r.present || !r.active || r.dead || !netFoe(i)) continue;
        float tb = rayCharacterHit(t, dir, r.b, 0.0f, d);
        if (tb > 0.0f) d = tb;
    }
    for (int i = 0; i < 3; ++i) out[i] = t[i] + dir[i] * d;
}

static void fireProjectile(const Player& p) {
    float target[3], f[3], m[3];
    aimPoint(p, target, f);
    // outro personagem pode usar o mesmo modelo (mesmo genero) e ter sido posado por ultimo: refaz a pose do jogador
    if (g_net != NET_NONE || (g_bot.active && g_bot.model == g_choice)) poseModel(g_models[g_choice], p.walkPhase, p.speed, g_time, smooth01(g_armBlend));
    muzzleWorld(p, g_choice, m);
    float d[3] = { target[0] - m[0], target[1] - m[1], target[2] - m[2] };
    float dl = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (dl < 0.5f) { d[0] = f[0]; d[1] = f[1]; d[2] = f[2]; }
    norm3(d);
    spawnProjectile(0, p, m, d);
    if (g_net != NET_NONE) netSendShot(m, d);
}

static bool projectileInside(const float p[3]) {
    if (p[1] <= 0.0f || p[1] >= g_ceil) return true;
    for (int i = 0; i < g_numObs; ++i) {
        const Obstacle& o = g_obs[i];
        if (std::fabs(p[0] - o.x) <= o.hw && std::fabs(p[2] - o.z) <= o.hd && p[1] <= o.h) return true;
    }
    return false;
}

static void updateProjectiles(const Player& pl, float dt) {
    float eye[3], f[3], s[3], u[3];
    cameraRig(pl, eye, f, s, u);
    float aspect = (float)g_width / (float)(g_height > 0 ? g_height : 1);
    float halfH = std::tan(30.0f * PI / 180.0f), halfW = halfH * aspect;

    size_t keep = 0;
    for (size_t i = 0; i < g_proj.size(); ++i) {
        Projectile b = g_proj[i];
        bool dead = false;
        int steps = (int)std::ceil(BULLET_SPEED * dt / 0.2f);   // passos curtos: nao atravessa caixas finas
        if (steps < 1) steps = 1;
        float h = dt / steps;
        for (int k = 0; k < steps && !dead; ++k) {
            float prev[3] = { b.p[0], b.p[1], b.p[2] };
            for (int c = 0; c < 3; ++c) b.p[c] += b.v[c] * h;
            b.dist += BULLET_SPEED * h;
            if (projectileInside(b.p)) { addDecal(prev, b.p); dead = true; }      // bateu: deixa a marca
            else if (b.dist > BULLET_RANGE) dead = true;
            else if (b.owner == 0 && g_bot.active && !g_bot.dead && insideCharacter(b.p, g_bot.b)) {   // acertou o bot
                botDamage(1);
                spawnBodyHit(b.p);
                dead = true;
            } else if (b.owner == 1 && !g_playerDead && insideCharacter(b.p, g_player)) {              // o bot acertou o jogador
                if (g_invuln <= 0.0f) { g_hurtDir[0] = -b.v[0]; g_hurtDir[1] = -b.v[2]; }
                damagePlayer(1);
                spawnBodyHit(b.p);
                dead = true;
            } else if (b.owner == 0 && g_net != NET_NONE) {                                           // meu tiro na rede
                int id = remoteAt(b.p, g_myId, g_myTeam);
                if (id >= 0) { remoteDamage(id); spawnBodyHit(b.p); dead = true; }
            } else if (b.owner >= 2) {                  // tiro de outro jogador: so o desenho (o dano chega como aviso dele)
                int oid = b.owner - 2, oteam = g_rp[oid].team;
                bool hitsMe = !g_playerDead && (g_mode == MODE_FFA || oteam != g_myTeam) && insideCharacter(b.p, g_player);
                if (hitsMe || remoteAt(b.p, oid, oteam) >= 0) { spawnBodyHit(b.p); dead = true; }
            }
        }
        if (!dead && b.owner == 0 && g_width > 0 && g_height > 0) {                         // saiu do campo de visao da camera? (so os tiros do jogador)
            float v[3] = { b.p[0] - eye[0], b.p[1] - eye[1], b.p[2] - eye[2] };
            float zc = v[0] * f[0] + v[1] * f[1] + v[2] * f[2];
            float xc = v[0] * s[0] + v[1] * s[1] + v[2] * s[2];
            float yc = v[0] * u[0] + v[1] * u[1] + v[2] * u[2];
            if (zc < 0.1f || std::fabs(xc) > zc * halfW || std::fabs(yc) > zc * halfH) dead = true;
        }
        if (!dead) g_proj[keep++] = b;
    }
    g_proj.resize(keep);
}

// ---- rede local (LAN): sala com ate MAX_PLAYERS jogadores
// UDP sem conexao. Quem cria a sala (o anfitriao, jogador 0) escuta na porta NET_PORT e e o centro: cada convidado so
// conversa com ele. Quem procura sala manda um "procurando" em broadcast e os anfitrioes respondem com nome, lotacao,
// modo e fase. Ao entrar (HELLO / WELCOME) o convidado recebe um numero (1..7) e uma equipe. Depois, ~30 vezes por
// segundo, cada convidado manda o proprio estado ao anfitriao, e o anfitriao manda a cada um o estado de todos (WORLD),
// junto com o modo, a fase da sala e o numero da partida -- e assim que todos sabem quando ir para a partida ou voltar
// para a sala. Tiros, acertos e eliminacoes sao eventos numerados que vao junto ate o outro lado confirmar (nao se
// perdem); o anfitriao repassa cada um a quem precisa. Quem atira decide se acertou e avisa so o atingido; o atingido
// tira a propria vida e, ao ser eliminado, avisa todos quem o eliminou (e quem eliminou soma o abate).
static const unsigned short NET_PORT = 27015;
static const unsigned char NET_VERSION = 5;
static const float NET_SEND_INTERVAL = 1.0f / 30.0f;
static const float NET_TIMEOUT = 6.0f;
enum NetMsg { NM_FIND = 1, NM_HERE, NM_HELLO, NM_WELCOME, NM_STATE, NM_BYE, NM_WORLD, NM_FULL };
enum NetEv { EV_SHOT = 1, EV_HIT, EV_KILL };
static const int NET_MAX_EV = 16;
#pragma pack(push, 1)
struct NetHeader { char magic[4]; unsigned char type, version; };
struct NetHello { NetHeader h; char name[MAX_NAME + 1]; unsigned char model, hair, color, beard, outfit; };
struct NetHere { NetHeader h; char name[MAX_NAME + 1]; unsigned char players, maxPlayers, mode, phase, map; };
struct NetWelcome { NetHeader h; unsigned char id, team, mode, phase, match; };
struct NetEvent { unsigned seq; unsigned char type, src, dst; float f[6]; };   // src: quem fez; dst: o alvo (acerto/abate)
struct NetBody {                        // o que cada jogador conta de si mesmo
    float x, y, z, face, vx, vz, speed, arm, aim;
    unsigned char hp, dead, reload, team;   // reload: 0 = nao esta recarregando; 1..255 = progresso
    unsigned short kills, deaths;
    unsigned char match;                // em qual partida ele esta (os numeros acima so valem nela)
};
struct NetState { NetHeader h; NetBody b; unsigned ack; unsigned char nEv; NetEvent ev[NET_MAX_EV]; };   // convidado -> anfitriao
struct NetSlot { unsigned char used, model, hair, color, beard; char name[MAX_NAME + 1]; NetBody b; };
struct NetWorld {                       // anfitriao -> convidado
    NetHeader h;
    unsigned char mode, phase, match, winner, limit, map;
    unsigned ack;                       // ultimo evento deste convidado que o anfitriao ja aplicou
    NetSlot pl[MAX_PLAYERS];            // todos os jogadores da sala, pelo numero (o 0 e o anfitriao)
    unsigned char nEv;
    NetEvent ev[NET_MAX_EV];
};
#pragma pack(pop)
static const int NET_STATE_BASE = (int)offsetof(NetState, ev);
static const int NET_WORLD_BASE = (int)offsetof(NetWorld, ev);

struct FoundHost { sockaddr_in addr; char name[MAX_NAME + 1]; int players, mode, phase, map; float age; };
struct NetLink {                        // conversa com o outro lado: meus eventos ainda nao confirmados e o ultimo dele
    bool used = false;
    sockaddr_in addr;
    float silence = 0.0f;
    std::vector<NetEvent> outbox;
    unsigned nextEv = 1, lastEvIn = 0;
};
static SOCKET g_sock = INVALID_SOCKET;
static bool  g_wsaReady = false;
static bool  g_connected = false;       // convidado: ja esta dentro da sala
static bool  g_netLost = false;         // a conexao acabou (o laco principal volta para o modo de jogo)
static bool  g_helloPending = false;    // pediu para entrar e espera a resposta
static bool  g_worldSeen = false;       // ja recebeu a lista de jogadores (dai em diante avisa quem entra)
static float g_netSendT = 0, g_netFindT = 0, g_netHelloT = 0, g_netHelloAge = 0;
static std::string g_netStatus;         // mensagem para o jogador (erro, conexao caiu...)
static std::vector<FoundHost> g_found;  // salas achadas na rede
static int   g_foundSel = 0;
static std::string g_ipText;            // IP digitado na tela de entrar
static NetLink g_link[MAX_PLAYERS];     // anfitriao: uma conversa por convidado, no numero dele ([0] sem uso)
static NetLink g_hostLink;              // convidado: a conversa com o anfitriao
static NetBody g_body[MAX_PLAYERS];     // anfitriao: ultimo estado recebido de cada convidado (repassado aos outros)
static int   g_teamHits = 0;            // avisos de acerto vindos de companheiro de equipe, ignorados (deve ficar 0)

static bool sameAddr(const sockaddr_in& a, const sockaddr_in& b) {
    return a.sin_addr.s_addr == b.sin_addr.s_addr && a.sin_port == b.sin_port;
}

static void netHeader(NetHeader& h, unsigned char type) {
    memcpy(h.magic, "M3DL", 4);
    h.type = type;
    h.version = NET_VERSION;
}

static void netSendTo(const sockaddr_in& to, const void* data, int n) {
    if (g_sock != INVALID_SOCKET) sendto(g_sock, (const char*)data, n, 0, (const sockaddr*)&to, sizeof(to));
}

static void netFillHello(NetHello& m, unsigned char type) {
    memset(&m, 0, sizeof(m));
    netHeader(m.h, type);
    std::string nm = displayName();
    strncpy(m.name, nm.c_str(), MAX_NAME);
    const Look& lk = g_look[g_choice];
    m.model = (unsigned char)g_choice;
    m.hair = (unsigned char)lk.hair; m.color = (unsigned char)lk.color;
    m.beard = lk.beard ? 1 : 0; m.outfit = lk.outfit ? 1 : 0;
}

// Os IPv4 desta maquina (para mostrar ao anfitriao o que digitar do outro lado, se a busca automatica falhar).
static std::string localIPs() {
    char host[256];
    if (gethostname(host, sizeof(host)) != 0) return "?";
    hostent* he = gethostbyname(host);
    std::string out;
    for (int i = 0; he && he->h_addr_list[i]; ++i) {
        in_addr a;
        memcpy(&a, he->h_addr_list[i], sizeof(a));
        if (!out.empty()) out += "   ";
        out += inet_ntoa(a);
    }
    return out.empty() ? "?" : out;
}

static int killLimit() { return g_killLimit > 0 ? g_killLimit : KILL_LIMIT[g_mode & 1]; }

// Quantos jogadores estao na sala, contando comigo.
static int netPlayers() {
    int n = 1;
    for (int i = 0; i < MAX_PLAYERS; ++i) if (i != g_myId && g_rp[i].present) ++n;
    return n;
}

static std::string playerName(int id) {
    if (id == g_myId) return displayName();
    if (id >= 0 && id < MAX_PLAYERS && g_rp[id].present) return g_rp[id].name;
    return "?";
}

static void feedAdd(const std::string& text, int kind) {
    FeedLine l;
    l.text = text;
    l.t = g_time;
    l.kind = kind;
    g_feed.push_back(l);
    if (g_feed.size() > 6) g_feed.erase(g_feed.begin());
}

static void addKillFeed(int killer, int victim) {
    int kind = killer == g_myId ? 1 : (victim == g_myId ? 2 : 0);
    if (killer >= 0 && killer < MAX_PLAYERS && killer != victim) feedAdd(playerName(killer) + "  eliminou  " + playerName(victim), kind);
    else feedAdd(playerName(victim) + "  foi eliminado", kind);
}

// Nome e aparencia de outro jogador (chegam no pedido para entrar ou na lista do anfitriao).
static void netSetIdentity(Bot& r, const char* name, int model, int hair, int color, int beard) {
    int m = model < 2 ? model : 0;
    if (!g_models[m].loaded) m = g_choice;
    r.model = m;
    r.look.hair = hair % NUM_HAIR;
    r.look.color = color % NUM_COLORS;
    r.look.beard = beard != 0;
    r.look.outfit = true;
    char nm[MAX_NAME + 1];
    memcpy(nm, name, MAX_NAME);
    nm[MAX_NAME] = 0;
    r.name = nm[0] ? nm : "Jogador";
    r.remote = true;
}

static void netClose() {
    if (g_sock != INVALID_SOCKET) {
        NetHeader h;
        netHeader(h, NM_BYE);
        for (int k = 0; k < 3; ++k) {                   // avisa que saiu (o anfitriao avisa todos: a sala fechou)
            if (g_net == NET_HOST) { for (int i = 1; i < MAX_PLAYERS; ++i) if (g_link[i].used) netSendTo(g_link[i].addr, &h, sizeof(h)); }
            else if (g_connected) netSendTo(g_hostLink.addr, &h, sizeof(h));
        }
        closesocket(g_sock);
    }
    g_sock = INVALID_SOCKET;
    g_net = NET_NONE;
    g_connected = g_helloPending = false;
    g_found.clear();
    for (int i = 0; i < MAX_PLAYERS; ++i) { g_link[i] = NetLink(); g_rp[i] = Bot(); }
    g_hostLink = NetLink();
    g_feed.clear();
    g_phase = PH_LOBBY;
    g_winner = -1;
    g_myId = 0;
}

// host: escuta na porta do jogo. loopbackOnly: so na propria maquina (o teste automatico usa, para nao abrir o firewall).
static bool netOpen(bool host, bool loopbackOnly = false) {
    netClose();
    if (!g_wsaReady) {
        WSADATA w;
        if (WSAStartup(MAKEWORD(2, 2), &w) != 0) { g_netStatus = "Erro ao iniciar a rede do Windows."; return false; }
        g_wsaReady = true;
    }
    g_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (g_sock == INVALID_SOCKET) { g_netStatus = "Erro ao criar a conexao de rede."; return false; }
    BOOL yes = TRUE;
    setsockopt(g_sock, SOL_SOCKET, SO_BROADCAST, (const char*)&yes, sizeof(yes));
    sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(loopbackOnly ? INADDR_LOOPBACK : INADDR_ANY);
    a.sin_port = htons(host ? NET_PORT : 0);
    if (bind(g_sock, (const sockaddr*)&a, sizeof(a)) == SOCKET_ERROR) {
        g_netStatus = host ? "A porta 27015 ja esta em uso (ja existe uma sala criada neste PC?)." : "Erro ao abrir a conexao de rede.";
        closesocket(g_sock);
        g_sock = INVALID_SOCKET;
        return false;
    }
    u_long nb = 1;
    ioctlsocket(g_sock, FIONBIO, &nb);
    g_net = host ? NET_HOST : NET_JOIN;
    g_netStatus.clear();
    g_netSendT = g_netFindT = 0.0f;
    g_foundSel = 0;
    g_phase = PH_LOBBY;
    g_matchId = 0;
    g_myMatch = -1;
    g_winner = -1;
    g_myId = 0;
    g_myTeam = 0;
    g_teamHits = 0;
    if (host) g_killLimit = 0;
    return true;
}

static void netConnect(const sockaddr_in& to) {
    g_hostLink = NetLink();
    g_hostLink.addr = to;
    g_helloPending = true;
    g_worldSeen = false;
    g_netHelloT = 0.0f;
    g_netHelloAge = 0.0f;
    g_netStatus = std::string("Conectando a ") + inet_ntoa(to.sin_addr) + "...";
}

// ---- eventos
static void linkQueue(NetLink& l, NetEvent e) {
    e.seq = l.nextEv++;
    l.outbox.push_back(e);
}

// Entrega um evento: o convidado manda ao anfitriao; o anfitriao manda a quem precisa (tiro e eliminacao: todos; acerto:
// so o atingido). except: de quem o anfitriao recebeu o evento (nao volta para ele).
static void netRoute(const NetEvent& e, int except) {
    if (g_net == NET_JOIN) { if (g_connected) linkQueue(g_hostLink, e); return; }
    if (g_net != NET_HOST) return;
    for (int i = 1; i < MAX_PLAYERS; ++i) {
        if (!g_link[i].used || i == except) continue;
        if (e.type == EV_HIT && e.dst != i) continue;
        linkQueue(g_link[i], e);
    }
}

static void netEmit(unsigned char type, int dst, const float f[6]) {
    if (g_net == NET_NONE) return;
    NetEvent e;
    memset(&e, 0, sizeof(e));
    e.type = type;
    e.src = (unsigned char)g_myId;
    e.dst = (unsigned char)dst;
    memcpy(e.f, f, sizeof(e.f));
    netRoute(e, -1);
}

static void netSendHit(int id) { float f[6] = { 0, 0, 0, 0, 0, 0 }; netEmit(EV_HIT, id, f); }
static void netSendShot(const float o[3], const float d[3]) { float f[6] = { o[0], o[1], o[2], d[0], d[1], d[2] }; netEmit(EV_SHOT, 255, f); }

static void netReportKill() {
    if (g_net == NET_NONE) return;
    int k = g_lastHitBy >= 0 && g_lastHitBy < MAX_PLAYERS && g_lastHitBy != g_myId ? g_lastHitBy : -1;
    float f[6] = { 0, 0, 0, 0, 0, 0 };
    netEmit(EV_KILL, k < 0 ? 255 : k, f);
    addKillFeed(k, g_myId);
}

// Um evento de outro jogador (ja na ordem certa).
static void netApplyEvent(const NetEvent& e) {
    int src = e.src;
    if (src >= MAX_PLAYERS || src == g_myId) return;
    Bot& r = g_rp[src];
    if (e.type == EV_SHOT) {
        if (!r.present || !r.active) return;
        float o[3] = { e.f[0], e.f[1], e.f[2] }, d[3] = { e.f[3], e.f[4], e.f[5] };
        spawnProjectile(2 + src, r.b, o, d);
        r.flash = 0.06f;
        r.recoil = 1.0f; r.shotHold = SHOT_HOLD;
    } else if (e.type == EV_HIT) {
        if (e.dst != g_myId || g_playerDead || g_phase != PH_PLAY) return;
        if (g_mode == MODE_TEAMS && r.team == g_myTeam) { ++g_teamHits; return; }
        if (g_invuln <= 0.0f && g_hp > 0) {
            g_hurtDir[0] = r.b.x - g_player.x; g_hurtDir[1] = r.b.z - g_player.z;
            g_lastHitBy = src;
        }
        damagePlayer(1);
    } else if (e.type == EV_KILL) {
        int killer = e.dst < MAX_PLAYERS ? e.dst : -1;
        if (killer == g_myId && g_phase == PH_PLAY) ++g_myKills;
        addKillFeed(killer, src);
    }
}

// ---- estado de cada jogador
static void netFillBody(NetBody& b) {
    const Player& p = g_player;
    b.x = p.x; b.y = p.y; b.z = p.z; b.face = p.face; b.vx = p.vx; b.vz = p.vz; b.speed = p.speed;
    b.arm = g_armBlend;
    b.aim = playerAimPitch();
    b.reload = g_reloadTimer > 0.0f && g_reloadTotal > 0.0f ? (unsigned char)(1.0f + 254.0f * (1.0f - g_reloadTimer / g_reloadTotal)) : 0;
    b.hp = (unsigned char)g_hp;
    b.dead = g_playerDead ? 1 : 0;
    b.team = (unsigned char)g_myTeam;
    b.kills = (unsigned short)g_myKills;
    b.deaths = (unsigned short)g_scoreBot;
    b.match = (unsigned char)g_myMatch;
}

static void netApplyBody(Bot& r, const NetBody& s) {
    r.team = s.team & 1;
    bool inMatch = g_phase != PH_LOBBY && s.match == (unsigned char)g_matchId;
    r.kills = inMatch ? s.kills : 0;
    r.deaths = inMatch ? s.deaths : 0;
    if (!inMatch) { r.active = false; return; }        // na sala de espera (ou ainda entrando na partida): fora do mapa
    bool first = !r.active;
    r.active = true;
    float jump = std::fabs(s.x - r.b.x) + std::fabs(s.z - r.b.z);
    if (first || jump > 6.0f || (r.dead && !s.dead)) {   // primeira vez ou renasceu: aparece direto no lugar
        r.b.x = s.x; r.b.y = s.y; r.b.z = s.z; r.b.face = s.face;
    }
    r.netX = s.x; r.netY = s.y; r.netZ = s.z; r.netVx = s.vx; r.netVz = s.vz;
    r.netFace = s.face; r.netSpeed = s.speed; r.netAge = 0.0f;
    r.arm = s.arm;
    r.aimPitch = s.aim;
    r.reloadProg = s.reload ? (float)(s.reload - 1) / 254.0f : -1.0f;
    r.hp = s.hp;
    if (s.dead && !r.dead) r.fall = 0.0f;
    r.dead = s.dead != 0;
}

// ---- anfitriao
static int hostFindLink(const sockaddr_in& a) {
    for (int i = 1; i < MAX_PLAYERS; ++i) if (g_link[i].used && sameAddr(g_link[i].addr, a)) return i;
    return -1;
}

static void hostRemove(int i, const char* why) {
    std::string nm = g_rp[i].name;
    g_link[i] = NetLink();
    g_rp[i] = Bot();
    memset(&g_body[i], 0, sizeof(NetBody));
    feedAdd(nm + why, 0);
}

static void hostSendWorld(int i) {
    NetWorld w;
    memset(&w, 0, sizeof(w));
    netHeader(w.h, NM_WORLD);
    w.mode = (unsigned char)g_mode;
    w.phase = (unsigned char)g_phase;
    w.match = (unsigned char)g_matchId;
    w.winner = g_winner < 0 ? 255 : (unsigned char)g_winner;
    w.limit = (unsigned char)killLimit();
    w.map = (unsigned char)g_map;
    NetLink& l = g_link[i];
    w.ack = l.lastEvIn;
    NetSlot& me = w.pl[0];
    const Look& lk = g_look[g_choice];
    me.used = 1;
    me.model = (unsigned char)g_choice;
    me.hair = (unsigned char)lk.hair; me.color = (unsigned char)lk.color; me.beard = lk.beard ? 1 : 0;
    strncpy(me.name, displayName().c_str(), MAX_NAME);
    netFillBody(me.b);
    for (int k = 1; k < MAX_PLAYERS; ++k) {
        const Bot& r = g_rp[k];
        if (!r.present) continue;
        NetSlot& sl = w.pl[k];
        sl.used = 1;
        sl.model = (unsigned char)r.model;
        sl.hair = (unsigned char)r.look.hair; sl.color = (unsigned char)r.look.color; sl.beard = r.look.beard ? 1 : 0;
        strncpy(sl.name, r.name.c_str(), MAX_NAME);
        sl.b = g_body[k];
    }
    int n = (int)std::min(l.outbox.size(), (size_t)NET_MAX_EV);
    w.nEv = (unsigned char)n;
    for (int k = 0; k < n; ++k) w.ev[k] = l.outbox[(size_t)k];
    netSendTo(l.addr, &w, NET_WORLD_BASE + n * (int)sizeof(NetEvent));
}

// Abates por equipe (so os da partida atual).
static void netTeamKills(int out[2]) {
    out[0] = out[1] = 0;
    out[g_myTeam & 1] += g_myKills;
    for (int i = 0; i < MAX_PLAYERS; ++i)
        if (i != g_myId && g_rp[i].present) out[g_rp[i].team & 1] += g_rp[i].kills;
}

// O anfitriao encerra a partida quando alguem (ou uma equipe) chega a meta; depois de END_TIME todos voltam para a sala.
static void hostCheckEnd(float dt) {
    if (g_phase == PH_PLAY) {
        int lim = killLimit(), best = g_myId, bestK = g_myKills, tk[2];
        netTeamKills(tk);
        for (int i = 0; i < MAX_PLAYERS; ++i)
            if (i != g_myId && g_rp[i].present && g_rp[i].kills > bestK) { bestK = g_rp[i].kills; best = i; }
        int win = -1;
        if (g_mode == MODE_TEAMS) { if (tk[0] >= lim) win = 0; else if (tk[1] >= lim) win = 1; }
        else if (bestK >= lim) win = best;
        if (win >= 0) { g_phase = PH_END; g_winner = win; g_endTimer = END_TIME; }
    } else if (g_phase == PH_END) {
        g_endTimer -= dt;
        if (g_endTimer <= 0.0f) { g_phase = PH_LOBBY; g_winner = -1; }
    }
}

// Enter na sala: comeca a partida para todos (precisa de 2 jogadores; no modo Equipes, alguem em cada equipe).
static bool hostStartMatch() {
    int cnt[2] = { 0, 0 };
    cnt[g_myTeam & 1]++;
    for (int i = 0; i < MAX_PLAYERS; ++i) if (i != g_myId && g_rp[i].present) cnt[g_rp[i].team & 1]++;
    if (cnt[0] + cnt[1] < 2) { g_netStatus = "Precisa de pelo menos 2 jogadores na sala."; return false; }
    if (g_mode == MODE_TEAMS && (cnt[0] == 0 || cnt[1] == 0)) {
        g_netStatus = "No modo Equipes, cada equipe precisa de pelo menos 1 jogador (T troca a sua).";
        return false;
    }
    g_netStatus.clear();
    g_matchId = g_matchId % 254 + 1;
    g_phase = PH_PLAY;
    g_winner = -1;
    return true;
}

// ---- convidado
static void clientSendState() {
    NetState s;
    memset(&s, 0, sizeof(s));
    netHeader(s.h, NM_STATE);
    netFillBody(s.b);
    s.ack = g_hostLink.lastEvIn;
    int n = (int)std::min(g_hostLink.outbox.size(), (size_t)NET_MAX_EV);
    s.nEv = (unsigned char)n;
    for (int i = 0; i < n; ++i) s.ev[i] = g_hostLink.outbox[(size_t)i];
    netSendTo(g_hostLink.addr, &s, NET_STATE_BASE + n * (int)sizeof(NetEvent));
}

static void clientApplyWorld(const NetWorld& w, int nEv) {
    NetLink& l = g_hostLink;
    while (!l.outbox.empty() && l.outbox.front().seq <= w.ack) l.outbox.erase(l.outbox.begin());   // o anfitriao ja recebeu
    g_mode = w.mode & 1;
    g_phase = w.phase <= PH_END ? w.phase : PH_LOBBY;
    g_matchId = w.match;
    g_winner = w.winner == 255 ? -1 : w.winner;
    g_killLimit = w.limit;
    g_map = w.map < NUM_MAPS ? w.map : MAP_GALPAO;      // o mapa e o do anfitriao (ensureWorld monta)
    for (int k = 0; k < MAX_PLAYERS; ++k) {
        if (k == g_myId) continue;
        const NetSlot& sl = w.pl[k];
        Bot& r = g_rp[k];
        if (!sl.used) {
            if (r.present) { feedAdd(r.name + " saiu da sala", 0); r = Bot(); }
            continue;
        }
        if (!r.present) {
            r = Bot();
            r.present = true;
            netSetIdentity(r, sl.name, sl.model, sl.hair, sl.color, sl.beard);
            if (g_worldSeen) feedAdd(r.name + " entrou na sala", 0);
        }
        netApplyBody(r, sl.b);
    }
    g_worldSeen = true;
    for (int i = 0; i < nEv; ++i) {                     // eventos novos, em ordem
        const NetEvent& e = w.ev[i];
        if (e.seq != l.lastEvIn + 1) continue;
        l.lastEvIn = e.seq;
        netApplyEvent(e);
    }
}

static void netPoll(float dt) {
    if (g_sock == INVALID_SOCKET) return;
    char buf[2048];
    for (;;) {
        sockaddr_in from;
        int fl = sizeof(from);
        int n = recvfrom(g_sock, buf, sizeof(buf), 0, (sockaddr*)&from, &fl);
        if (n == SOCKET_ERROR) {
            int err = WSAGetLastError();
            if (err == WSAECONNRESET || err == WSAEMSGSIZE) continue;   // resto de pacote antigo: ignora
            break;                                                      // nada mais para ler
        }
        if (n < (int)sizeof(NetHeader)) continue;
        const NetHeader* h = (const NetHeader*)buf;
        if (memcmp(h->magic, "M3DL", 4) != 0) continue;
        if (h->version != NET_VERSION) {                // jogos de versoes diferentes nao se entendem: avisa em vez de ignorar
            if (g_net == NET_JOIN && !g_connected)
                g_netStatus = "Uma sala na rede esta com outra versao do jogo. Use a mesma versao em todos os PCs.";
            continue;
        }

        switch (h->type) {
        case NM_FIND:                                   // alguem procurando sala: o anfitriao responde
            if (g_net == NET_HOST) {
                NetHere m;
                memset(&m, 0, sizeof(m));
                netHeader(m.h, NM_HERE);
                strncpy(m.name, displayName().c_str(), MAX_NAME);
                m.players = (unsigned char)netPlayers();
                m.maxPlayers = (unsigned char)MAX_PLAYERS;
                m.mode = (unsigned char)g_mode;
                m.phase = (unsigned char)g_phase;
                m.map = (unsigned char)g_map;
                netSendTo(from, &m, sizeof(m));
            }
            break;
        case NM_HERE:
            if (g_net == NET_JOIN && !g_connected && n >= (int)sizeof(NetHere)) {
                const NetHere* m = (const NetHere*)buf;
                size_t i = 0;
                while (i < g_found.size() && !sameAddr(g_found[i].addr, from)) ++i;
                if (i == g_found.size()) { FoundHost fh; memset(&fh, 0, sizeof(fh)); fh.addr = from; g_found.push_back(fh); }
                memcpy(g_found[i].name, m->name, MAX_NAME);
                g_found[i].name[MAX_NAME] = 0;
                g_found[i].players = m->players;
                g_found[i].mode = m->mode & 1;
                g_found[i].phase = m->phase;
                g_found[i].map = m->map < NUM_MAPS ? m->map : 0;
                g_found[i].age = 0.0f;
            }
            break;
        case NM_HELLO:                                  // pedido para entrar na sala
            if (g_net == NET_HOST && n >= (int)sizeof(NetHello)) {
                int i = hostFindLink(from);
                if (i < 0) {
                    for (int k = 1; k < MAX_PLAYERS && i < 0; ++k) if (!g_link[k].used) i = k;
                    if (i < 0) { NetHeader fh; netHeader(fh, NM_FULL); netSendTo(from, &fh, sizeof(fh)); break; }
                    const NetHello* m = (const NetHello*)buf;
                    g_link[i] = NetLink();
                    g_link[i].used = true;
                    g_link[i].addr = from;
                    g_rp[i] = Bot();
                    g_rp[i].present = true;
                    netSetIdentity(g_rp[i], m->name, m->model, m->hair, m->color, m->beard);
                    int cnt[2] = { 0, 0 };                  // entra na equipe com menos gente
                    cnt[g_myTeam & 1]++;
                    for (int k = 0; k < MAX_PLAYERS; ++k) if (k != i && k != g_myId && g_rp[k].present) cnt[g_rp[k].team & 1]++;
                    g_rp[i].team = cnt[1] < cnt[0] ? 1 : 0;
                    memset(&g_body[i], 0, sizeof(NetBody));
                    g_body[i].team = (unsigned char)g_rp[i].team;
                    g_body[i].match = 255;
                    feedAdd(g_rp[i].name + " entrou na sala", 0);
                }
                NetWelcome wm;
                memset(&wm, 0, sizeof(wm));
                netHeader(wm.h, NM_WELCOME);
                wm.id = (unsigned char)i;
                wm.team = (unsigned char)g_rp[i].team;
                wm.mode = (unsigned char)g_mode;
                wm.phase = (unsigned char)g_phase;
                wm.match = (unsigned char)g_matchId;
                netSendTo(from, &wm, sizeof(wm));
            }
            break;
        case NM_WELCOME:                                // o anfitriao aceitou
            if (g_net == NET_JOIN && g_helloPending && sameAddr(from, g_hostLink.addr) && n >= (int)sizeof(NetWelcome)) {
                const NetWelcome* wm = (const NetWelcome*)buf;
                g_helloPending = false;
                g_connected = true;
                g_myId = wm->id % MAX_PLAYERS;
                g_myTeam = wm->team & 1;
                g_mode = wm->mode & 1;
                g_phase = wm->phase <= PH_END ? wm->phase : PH_LOBBY;
                g_matchId = wm->match;
                g_hostLink.silence = 0.0f;
                g_netStatus.clear();
            }
            break;
        case NM_FULL:
            if (g_net == NET_JOIN && g_helloPending && sameAddr(from, g_hostLink.addr)) {
                g_helloPending = false;
                g_netStatus = "Essa sala esta cheia (8 jogadores).";
            }
            break;
        case NM_STATE:                                  // estado de um convidado
            if (g_net == NET_HOST && n >= NET_STATE_BASE) {
                int i = hostFindLink(from);
                if (i < 0) break;
                NetState st;
                memset(&st, 0, sizeof(st));
                memcpy(&st, buf, (size_t)std::min(n, (int)sizeof(st)));
                int maxEv = (n - NET_STATE_BASE) / (int)sizeof(NetEvent);
                if (st.nEv > maxEv) st.nEv = (unsigned char)maxEv;
                NetLink& l = g_link[i];
                l.silence = 0.0f;
                while (!l.outbox.empty() && l.outbox.front().seq <= st.ack) l.outbox.erase(l.outbox.begin());
                g_body[i] = st.b;
                netApplyBody(g_rp[i], st.b);
                for (int k = 0; k < st.nEv; ++k) {
                    NetEvent e = st.ev[k];
                    if (e.seq != l.lastEvIn + 1) continue;
                    l.lastEvIn = e.seq;
                    e.src = (unsigned char)i;           // cada um so fala por si
                    netApplyEvent(e);
                    netRoute(e, i);
                }
            }
            break;
        case NM_WORLD:                                  // a sala inteira, vinda do anfitriao
            if (g_net == NET_JOIN && g_connected && sameAddr(from, g_hostLink.addr) && n >= NET_WORLD_BASE) {
                static NetWorld w;
                memset(&w, 0, sizeof(w));
                memcpy(&w, buf, (size_t)std::min(n, (int)sizeof(w)));
                int nEv = std::min((int)w.nEv, (n - NET_WORLD_BASE) / (int)sizeof(NetEvent));
                g_hostLink.silence = 0.0f;
                clientApplyWorld(w, nEv);
            }
            break;
        case NM_BYE:
            if (g_net == NET_HOST) {
                int i = hostFindLink(from);
                if (i >= 0) hostRemove(i, " saiu da sala");
            } else if (g_connected && sameAddr(from, g_hostLink.addr)) {
                g_netLost = true;
                g_netStatus = "O anfitri\xE3o fechou a sala.";
            }
            break;
        }
    }

    if (g_net == NET_JOIN && !g_connected) {
        g_netFindT -= dt;
        if (g_netFindT <= 0.0f) {                       // procura salas: broadcast na rede e tambem neste PC
            NetHeader fh;
            netHeader(fh, NM_FIND);
            sockaddr_in to;
            memset(&to, 0, sizeof(to));
            to.sin_family = AF_INET;
            to.sin_port = htons(NET_PORT);
            to.sin_addr.s_addr = htonl(INADDR_BROADCAST);
            netSendTo(to, &fh, sizeof(fh));
            to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            netSendTo(to, &fh, sizeof(fh));
            g_netFindT = 0.5f;
        }
        for (size_t i = 0; i < g_found.size();) {      // some da lista quem parou de responder
            g_found[i].age += dt;
            if (g_found[i].age > 3.0f) g_found.erase(g_found.begin() + (long)i); else ++i;
        }
        if (g_foundSel >= (int)g_found.size()) g_foundSel = g_found.empty() ? 0 : (int)g_found.size() - 1;
        if (g_helloPending) {
            g_netHelloT -= dt;
            g_netHelloAge += dt;
            if (g_netHelloT <= 0.0f) { NetHello m; netFillHello(m, NM_HELLO); netSendTo(g_hostLink.addr, &m, sizeof(m)); g_netHelloT = 0.3f; }
            if (g_netHelloAge > 5.0f) {
                g_helloPending = false;
                g_netStatus = std::string("Sem resposta de ") + inet_ntoa(g_hostLink.addr.sin_addr) + ".";
            }
        }
    } else if (g_net == NET_JOIN) {
        g_hostLink.silence += dt;
        if (g_hostLink.silence > NET_TIMEOUT && !g_netLost) { g_netLost = true; g_netStatus = "A conex\xE3o com o anfitri\xE3o caiu."; }
        g_netSendT -= dt;
        if (g_netSendT <= 0.0f) { clientSendState(); g_netSendT = NET_SEND_INTERVAL; }
    } else if (g_net == NET_HOST) {
        for (int i = 1; i < MAX_PLAYERS; ++i) {
            if (!g_link[i].used) continue;
            g_link[i].silence += dt;
            if (g_link[i].silence > NET_TIMEOUT) hostRemove(i, " caiu (sem resposta)");
        }
        hostCheckEnd(dt);
        g_netSendT -= dt;
        if (g_netSendT <= 0.0f) {
            for (int i = 1; i < MAX_PLAYERS; ++i) if (g_link[i].used) hostSendWorld(i);
            g_netSendT = NET_SEND_INTERVAL;
        }
    }
}

// Outro jogador entre um pacote e outro: segue o ultimo estado recebido (um pouco adiante pela velocidade), suavizado.
static void remoteUpdate(Bot& bt, float dt) {
    Player& b = bt.b;
    bt.netAge += dt;
    float ext = std::min(bt.netAge, 0.2f);
    float k = 1.0f - std::exp(-14.0f * dt);
    b.x += (bt.netX + bt.netVx * ext - b.x) * k;
    b.z += (bt.netZ + bt.netVz * ext - b.z) * k;
    b.y += (bt.netY - b.y) * k;
    b.face += angleDiff(bt.netFace, b.face) * k;
    b.vx = bt.netVx; b.vz = bt.netVz;
    b.speed = bt.netSpeed;
    b.walkPhase += gaitCadence(b.speed) * dt;
    b.walkPhase -= std::floor(b.walkPhase);
    if (bt.hurt > 0.0f) bt.hurt -= dt * 3.0f;
    if (bt.flash > 0.0f) bt.flash -= dt;
    if (bt.dead) bt.fall = std::min(bt.fall + dt / 0.6f, 1.0f);
    bool inAir = bt.netY > groundHeightAt(b.x, b.z, bt.netY) + 0.12f;
    if (!inAir && b.air > 0.6f) b.land = 1.0f;
    b.land = std::max(0.0f, b.land - dt / 0.32f);
    b.air += ((inAir ? 1.0f : 0.0f) - b.air) * (1.0f - std::exp(-16.0f * dt));
}

static void remotesUpdate(float dt) {
    for (int i = 0; i < MAX_PLAYERS; ++i) {
        Bot& r = g_rp[i];
        if (i == g_myId || !r.present || !r.active) continue;
        if (r.recoil > 0.0f) r.recoil = std::max(0.0f, r.recoil - dt / 0.14f);
        fireBlendStep(r.shotHold, r.fireBlend, r.reloadProg >= 0.0f, dt);
        remoteUpdate(r, dt);
    }
}

// ---- navegacao do bot
// A sala vira uma grade de celulas de 2,5. Celulas muito perto de paredes/caixas sao bloqueadas. Uma busca em largura a
// partir da celula-alvo da a distancia de cada celula ate o alvo; o bot anda "descendo" essas distancias.
static const float NAV_CELL = 2.5f;
static const int NAV_W = 60, NAV_H = 44;                 // 150 x 110
static bool g_navFree[NAV_H][NAV_W];
static int  g_navDist[NAV_H][NAV_W];
static bool g_navBuilt = false;
static const int NDX[8] = { 1, -1, 0, 0, 1, 1, -1, -1 };
static const int NDZ[8] = { 0, 0, 1, -1, 1, -1, 1, -1 };

static float navX(int ix) { return -ROOM_X + (ix + 0.5f) * NAV_CELL; }
static float navZ(int iz) { return -ROOM_Z + (iz + 0.5f) * NAV_CELL; }
static int navIX(float x) { int i = (int)std::floor((x + ROOM_X) / NAV_CELL); return i < 0 ? 0 : (i >= NAV_W ? NAV_W - 1 : i); }
static int navIZ(float z) { int i = (int)std::floor((z + ROOM_Z) / NAV_CELL); return i < 0 ? 0 : (i >= NAV_H ? NAV_H - 1 : i); }

static bool blockedAt(float x, float z, float pad) {
    for (int i = 0; i < g_numObs; ++i) {
        const Obstacle& o = g_obs[i];
        if (std::fabs(x - o.x) < o.hw + pad && std::fabs(z - o.z) < o.hd + pad) return true;
    }
    return false;
}

static void buildNav() {
    for (int iz = 0; iz < NAV_H; ++iz)
        for (int ix = 0; ix < NAV_W; ++ix)
            g_navFree[iz][ix] = !blockedAt(navX(ix), navZ(iz), PLAYER_RADIUS + 0.35f);
    g_navBuilt = true;
}

static bool lineFree(float ax, float az, float bx, float bz) {
    float len = std::sqrt((bx - ax) * (bx - ax) + (bz - az) * (bz - az));
    int steps = (int)std::ceil(len / 0.5f);
    for (int i = 1; i <= steps; ++i) {
        float t = (float)i / (float)steps;
        if (blockedAt(ax + (bx - ax) * t, az + (bz - az) * t, PLAYER_RADIUS + 0.1f)) return false;
    }
    return true;
}

static bool navStepOk(int ix, int iz, int k) {
    int nx = ix + NDX[k], nz = iz + NDZ[k];
    if (nx < 0 || nz < 0 || nx >= NAV_W || nz >= NAV_H || !g_navFree[nz][nx]) return false;
    if (k >= 4 && (!g_navFree[iz][nx] || !g_navFree[nz][ix])) return false;       // diagonal nao passa raspando em quina
    return true;
}

static void navField(int gx, int gz) {
    for (int iz = 0; iz < NAV_H; ++iz) for (int ix = 0; ix < NAV_W; ++ix) g_navDist[iz][ix] = -1;
    if (!g_navFree[gz][gx]) return;
    static int qx[NAV_W * NAV_H], qz[NAV_W * NAV_H];
    int head = 0, tail = 0;
    g_navDist[gz][gx] = 0;
    qx[tail] = gx; qz[tail] = gz; ++tail;
    while (head < tail) {
        int cx = qx[head], cz = qz[head]; ++head;
        for (int k = 0; k < 8; ++k) {
            if (!navStepOk(cx, cz, k)) continue;
            int nx = cx + NDX[k], nz = cz + NDZ[k];
            if (g_navDist[nz][nx] >= 0) continue;
            g_navDist[nz][nx] = g_navDist[cz][cx] + 1;
            qx[tail] = nx; qz[tail] = nz; ++tail;
        }
    }
}

static bool nearestFree(float x, float z, int& ix, int& iz) {
    int cx = navIX(x), cz = navIZ(z);
    float best = 1e9f;
    bool found = false;
    for (int r = 0; r <= 4 && !found; ++r)
        for (int dz = -r; dz <= r; ++dz)
            for (int dx = -r; dx <= r; ++dx) {
                if ((dx < 0 ? -dx : dx) != r && (dz < 0 ? -dz : dz) != r) continue;
                int nx = cx + dx, nz = cz + dz;
                if (nx < 0 || nz < 0 || nx >= NAV_W || nz >= NAV_H || !g_navFree[nz][nx]) continue;
                float d = (navX(nx) - x) * (navX(nx) - x) + (navZ(nz) - z) * (navZ(nz) - z);
                if (d < best) { best = d; ix = nx; iz = nz; found = true; }
            }
    return found;
}

// Proximo ponto para onde andar: segue a descida das distancias e pega a celula mais distante que ainda se alcanca em linha reta.
static bool navNextTarget(const Player& b, float& tx, float& tz, bool& arrived) {
    int ix, iz;
    arrived = false;
    if (!nearestFree(b.x, b.z, ix, iz) || g_navDist[iz][ix] < 0) return false;
    if (g_navDist[iz][ix] == 0) { arrived = true; return true; }
    int cx = ix, cz = iz;
    bool first = true;
    for (int step = 0; step < 10 && g_navDist[cz][cx] > 0; ++step) {
        int want = g_navDist[cz][cx] - 1, nx = -1, nz = -1;
        for (int k = 0; k < 8; ++k)
            if (navStepOk(cx, cz, k) && g_navDist[cz + NDZ[k]][cx + NDX[k]] == want) { nx = cx + NDX[k]; nz = cz + NDZ[k]; break; }
        if (nx < 0) break;
        cx = nx; cz = nz;
        float wx = navX(cx), wz = navZ(cz);
        if (first) { tx = wx; tz = wz; first = false; }
        else if (lineFree(b.x, b.z, wx, wz)) { tx = wx; tz = wz; }
    }
    return !first;
}

// O bot enxerga o jogador se nenhuma parede/caixa esta entre os olhos dele e o peito do jogador.
static bool lineOfSight(float ax, float ay, float az, float bx, float by, float bz) {
    float a[3] = { ax, ay, az }, b[3] = { bx, by, bz }, h[3], n[3];
    return !segmentHit(a, b, h, n);
}

static void botStartReload() {
    Bot& bt = g_bot;
    bt.reloadTotal = RELOAD_TIME * (float)(MAG_SIZE - bt.ammo) / (float)MAG_SIZE;
    bt.reloadTimer = bt.reloadTotal;
}

// Procura a celula mais proxima (por caminho) de onde o bot nao e visto pelo jogador e vai para la.
static void botPlanCover() {
    Bot& bt = g_bot;
    int bx, bz;
    if (!nearestFree(bt.b.x, bt.b.z, bx, bz)) return;
    navField(bx, bz);
    int best = 1000, gx = bx, gz = bz;
    for (int iz = 0; iz < NAV_H; ++iz)
        for (int ix = 0; ix < NAV_W; ++ix) {
            int d = g_navDist[iz][ix];
            if (d <= 0 || d >= best || d > 14) continue;
            if (!lineOfSight(navX(ix), 2.4f, navZ(iz), g_player.x, g_player.y + 2.0f, g_player.z)) { best = d; gx = ix; gz = iz; }
        }
    navField(gx, gz);
    bt.state = BS_COVER;
}

// Reposiciona: escolhe uma celula proxima (2 a 6 passos), de preferencia com visao do jogador.
static void botPlanMove() {
    Bot& bt = g_bot;
    int bx, bz;
    if (!nearestFree(bt.b.x, bt.b.z, bx, bz)) return;
    navField(bx, bz);
    int seeX[800], seeZ[800], anyX[800], anyZ[800], nSee = 0, nAny = 0;
    for (int iz = 0; iz < NAV_H; ++iz)
        for (int ix = 0; ix < NAV_W; ++ix) {
            int d = g_navDist[iz][ix];
            if (d < 2 || d > 6) continue;
            if (nAny < 800) { anyX[nAny] = ix; anyZ[nAny] = iz; ++nAny; }
            if (nSee < 800 && lineOfSight(navX(ix), 2.4f, navZ(iz), g_player.x, g_player.y + 2.0f, g_player.z)) {
                seeX[nSee] = ix; seeZ[nSee] = iz; ++nSee;
            }
        }
    if (nAny == 0) { bt.state = BS_ATTACK; return; }
    int gx, gz;
    if (nSee > 0) { int i = (int)(frand() * nSee) % nSee; gx = seeX[i]; gz = seeZ[i]; }
    else          { int i = (int)(frand() * nAny) % nAny; gx = anyX[i]; gz = anyZ[i]; }
    navField(gx, gz);
    bt.state = BS_MOVE;
    bt.stateT = 0.0f;
}

static void botRespawn() {
    Bot& bt = g_bot;
    if (!bt.active || bt.remote) return;
    // renasce longe do jogador, mas nao sempre no mesmo canto: sorteia entre os pontos a mais de 55 de distancia
    int farIdx[NUM_SPAWNS], nFar = 0, best = 0;
    float bd = -1.0f;
    for (int i = 0; i < NUM_SPAWNS; ++i) {
        float d = (SPAWNS[i][0] - g_player.x) * (SPAWNS[i][0] - g_player.x) + (SPAWNS[i][1] - g_player.z) * (SPAWNS[i][1] - g_player.z);
        if (d > bd) { bd = d; best = i; }
        if (d > 55.0f * 55.0f) farIdx[nFar++] = i;
    }
    if (nFar > 0) best = farIdx[(int)(frand() * nFar) % nFar];
    bt.b = Player();
    bt.b.x = SPAWNS[best][0];
    bt.b.z = SPAWNS[best][1];
    bt.b.face = std::atan2(g_player.x - bt.b.x, g_player.z - bt.b.z);
    bt.hp = MAX_HP; bt.ammo = MAG_SIZE;
    bt.reloadTimer = 0; bt.fireCd = 0; bt.flash = 0; bt.hurt = 0;
    bt.invuln = 1.0f;
    bt.dead = false; bt.deadTimer = 0; bt.fall = 0;
    bt.state = BS_CHASE; bt.stateT = 0; bt.burstWait = 0.8f; bt.burstLeft = 5; bt.repath = 0; bt.noSightT = 0;
    bt.lastX = g_player.x; bt.lastZ = g_player.z; bt.alertT = 0;   // comeca sabendo mais ou menos onde o jogador esta
}

static void botUpdate(float dt) {
    Bot& bt = g_bot;
    if (!bt.active) return;
    if (bt.recoil > 0.0f) bt.recoil = std::max(0.0f, bt.recoil - dt / 0.14f);
    fireBlendStep(bt.shotHold, bt.fireBlend, bt.reloadTimer > 0.0f, dt);
    Player& b = bt.b;
    const Player& pl = g_player;

    if (bt.hurt > 0.0f) bt.hurt -= dt * 3.0f;
    if (bt.invuln > 0.0f) bt.invuln -= dt;
    if (bt.flash > 0.0f) bt.flash -= dt;
    if (bt.fireCd > 0.0f) bt.fireCd -= dt;
    if (bt.reloadTimer > 0.0f) {
        bt.reloadTimer -= dt;
        if (bt.reloadTimer <= 0.0f) { bt.reloadTimer = 0.0f; bt.ammo = MAG_SIZE; }
    }

    if (bt.dead) {                                      // eliminado: cai e renasce depois
        bt.fall = bt.fall + dt / 0.6f > 1.0f ? 1.0f : bt.fall + dt / 0.6f;
        moveActor(b, 0, 0, false, 0.0f, dt, false);
        bt.deadTimer -= dt;
        if (bt.deadTimer <= 0.0f) botRespawn();
        return;
    }
    if (g_playerDead) { moveActor(b, 0, 0, false, 0.0f, dt, false); return; }

    float dxp = pl.x - b.x, dzp = pl.z - b.z;
    float dist = std::sqrt(dxp * dxp + dzp * dzp);
    // Visao: precisa de linha livre e o jogador na frente do bot (cerca de 220 graus). De perto ou em alerta, percebe de
    // qualquer lado. Fora disso o bot nao sabe onde o jogador esta: e isso que faz os esconderijos funcionarem.
    if (bt.alertT > 0.0f) bt.alertT -= dt;
    bool clear = lineOfSight(b.x, b.y + 2.4f, b.z, pl.x, pl.y + 2.0f, pl.z);
    if (g_worldMap == MAP_BRANCO && dist > 55.0f) clear = false;        // a nevoa tambem tapa a visao do bot
    bool seen = clear && (std::fabs(angleDiff(std::atan2(dxp, dzp), b.face)) < 1.9f || dist < 7.0f || bt.alertT > 0.0f);
    if (seen) { bt.lastX = pl.x; bt.lastZ = pl.z; }
    float wantPitch = seen ? std::atan2(b.y + 2.3f - (pl.y + 2.0f), std::max(dist, 1.0f)) : 0.0f;
    bt.aimPitch += (wantPitch - bt.aimPitch) * (1.0f - std::exp(-8.0f * dt));
    bt.noSightT = seen ? 0.0f : bt.noSightT + dt;
    bt.stateT += dt;

    // sem municao util: recarrega e se esconde enquanto isso
    if (bt.reloadTimer <= 0.0f && (bt.ammo == 0 || (!seen && bt.ammo < 10))) botStartReload();
    if (bt.reloadTimer > 0.0f && bt.state != BS_COVER) botPlanCover();

    bool moving = false;
    float dirx = 0.0f, dirz = 0.0f, speed = WALK_SPEED;
    bool arrived = false;
    float tx = 0.0f, tz = 0.0f;

    switch (bt.state) {
    case BS_CHASE: {
        bt.repath -= dt;
        if (bt.repath <= 0.0f) {                        // refaz o campo de distancias em direcao a ultima posicao conhecida
            int gx, gz;
            if (nearestFree(bt.lastX, bt.lastZ, gx, gz)) navField(gx, gz);
            bt.repath = 0.35f;
        }
        if (navNextTarget(b, tx, tz, arrived) && !arrived) {
            dirx = tx - b.x; dirz = tz - b.z; moving = true;
            float toGoal = std::sqrt((bt.lastX - b.x) * (bt.lastX - b.x) + (bt.lastZ - b.z) * (bt.lastZ - b.z));
            speed = toGoal > 18.0f ? RUN_SPEED : WALK_SPEED;
        } else if (!seen) {
            // chegou e o jogador nao esta la: procura em volta de onde ele deve estar, sem saber o ponto exato
            float a = frand() * 2.0f * PI, r = 5.0f + frand() * 9.0f;
            bt.lastX = clampf(pl.x + std::cos(a) * r, -ROOM_X + 2.0f, ROOM_X - 2.0f);
            bt.lastZ = clampf(pl.z + std::sin(a) * r, -ROOM_Z + 2.0f, ROOM_Z - 2.0f);
            bt.repath = 0.0f;
        }
        if (seen && dist < 42.0f && bt.reloadTimer <= 0.0f) {
            bt.state = BS_ATTACK; bt.stateT = 0.0f;
            bt.burstLeft = 4 + (int)(frand() * 5.0f); bt.burstWait = 0.3f + frand() * 0.3f;
        }
        break;
    }
    case BS_ATTACK: {
        if (bt.burstWait > 0.0f) bt.burstWait -= dt;
        else if (seen && bt.fireCd <= 0.0f && bt.reloadTimer <= 0.0f && bt.ammo > 0 &&
                 std::fabs(angleDiff(std::atan2(dxp, dzp), b.face)) < 0.3f) {
            float m[3];
            muzzleWorld(b, bt.model, m);
            float lead = dist / BULLET_SPEED * 0.6f;    // mira um pouco a frente de quem esta correndo
            float tgt[3] = { pl.x + pl.vx * lead, pl.y + 2.0f, pl.z + pl.vz * lead };
            float d[3] = { tgt[0] - m[0], tgt[1] - m[1], tgt[2] - m[2] };
            norm3(d);
            for (int i = 0; i < 3; ++i) d[i] += (frand() - 0.5f) * BOT_AIM_ERROR;   // erro de pontaria
            norm3(d);
            spawnProjectile(1, b, m, d);
            bt.recoil = 1.0f; bt.shotHold = SHOT_HOLD;
            bt.fireCd = FIRE_INTERVAL;
            bt.flash = 0.06f;
            --bt.ammo;
            if (--bt.burstLeft <= 0) {
                bt.burstWait = 0.6f + frand() * 0.8f;
                bt.burstLeft = 4 + (int)(frand() * 5.0f);
                if (frand() < 0.55f) botPlanMove();
            }
        }
        if (bt.noSightT > 0.6f || dist > 46.0f) { bt.state = BS_CHASE; bt.repath = 0.0f; }
        break;
    }
    case BS_MOVE: {
        if (navNextTarget(b, tx, tz, arrived) && !arrived) {
            dirx = tx - b.x; dirz = tz - b.z; moving = true; speed = WALK_SPEED;
        }
        if (arrived || bt.stateT > 3.0f) { bt.state = BS_ATTACK; bt.stateT = 0.0f; }
        if (bt.noSightT > 1.5f) { bt.state = BS_CHASE; bt.repath = 0.0f; }
        break;
    }
    case BS_COVER: {
        if (navNextTarget(b, tx, tz, arrived) && !arrived) {
            dirx = tx - b.x; dirz = tz - b.z; moving = true; speed = RUN_SPEED;
        }
        if (bt.reloadTimer <= 0.0f) { bt.state = BS_CHASE; bt.repath = 0.0f; }
        break;
    }
    }

    float dl = std::sqrt(dirx * dirx + dirz * dirz);
    if (moving && dl > 1e-4f) { dirx /= dl; dirz /= dl; } else moving = false;
    bool faceTarget = bt.state == BS_ATTACK || !moving;       // parado, olha para onde acha que o jogador esta
    if (faceTarget) b.face += angleDiff(std::atan2(bt.lastX - b.x, bt.lastZ - b.z), b.face) * (1.0f - std::exp(-12.0f * dt));
    moveActor(b, dirx, dirz, moving, speed, dt, !faceTarget);

    // nao atravessa o jogador
    float sx = b.x - pl.x, sz = b.z - pl.z, sd = std::sqrt(sx * sx + sz * sz), minD = 2.0f * PLAYER_RADIUS;
    if (sd < minD && sd > 1e-4f) { b.x = pl.x + sx / sd * minD; b.z = pl.z + sz / sd * minD; }
}

// Troca de mapa: monta as caixas, recalcula a luz e ajusta o fundo e a neblina. So faz algo se g_map mudou (a escolha
// na tela de modo de jogo ou na sala; na rede, o mapa que o anfitriao mandou).
static float g_bakeMs = 0.0f;       // quanto demorou a ultima montagem
static void ensureWorld() {
    if (g_worldMap == g_map) return;
    LARGE_INTEGER fq, t0, t1;
    QueryPerformanceFrequency(&fq);
    QueryPerformanceCounter(&t0);
    g_worldMap = g_map;
    initWorld();
    buildWorld();
    g_navBuilt = false;
    g_decalCount = g_decalNext = 0;
    g_parts.clear();
    g_proj.clear();
    static const float FOG[NUM_MAPS][6] = {                 // cor, inicio e fim da neblina
        { 0.08f, 0.09f, 0.11f, 1.0f, 45.0f, 210.0f },       // galpao: fundo escuro
        { 0.90f, 0.83f, 0.71f, 1.0f, 70.0f, 520.0f },       // deserto: poeira clara no horizonte
        { 0.96f, 0.96f, 0.96f, 1.0f, 5.0f, 80.0f } };       // sala branca: nevoa branca e fechada
    const float* fg = FOG[g_worldMap];
    glClearColor(fg[0], fg[1], fg[2], 1.0f);
    glFogfv(GL_FOG_COLOR, fg);
    glFogf(GL_FOG_START, fg[4]);
    glFogf(GL_FOG_END, fg[5]);
    QueryPerformanceCounter(&t1);
    g_bakeMs = (float)((double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)fq.QuadPart);
}

static void respawnPlayer() {
    g_playerDead = false;
    g_playerFall = 0.0f;
    g_hp = MAX_HP;
    g_ammo = MAG_SIZE;
    g_reloadTimer = 0.0f;
    g_invuln = 1.0f;
    g_hurtTimer = 0.0f;
    g_aimHold = 0.0f;
    // Contra o bot: sempre no mesmo lugar, ao sul do miolo. Na rede: no ponto mais longe do inimigo mais proximo (no modo
    // Equipes, so do lado da propria equipe: azul ao sul, vermelha ao norte) e nunca em cima de um companheiro. Sem
    // ninguem no mapa, cada jogador comeca num ponto diferente (pelo numero dele).
    float sx = 0.0f, sz = 25.0f;
    if (g_net != NET_NONE) {
        float best = -1.0f;
        for (int n = 0; n < NUM_SPAWNS; ++n) {
            int k = (n + g_myId * 5) % NUM_SPAWNS;
            if (g_mode == MODE_TEAMS && (SPAWNS[k][1] > 0.0f) != (g_myTeam == 0)) continue;
            float nearest = 1e9f;
            for (int i = 0; i < MAX_PLAYERS; ++i) {
                const Bot& r = g_rp[i];
                if (i == g_myId || !r.present || !r.active || r.dead) continue;
                float d = (SPAWNS[k][0] - r.b.x) * (SPAWNS[k][0] - r.b.x) + (SPAWNS[k][1] - r.b.z) * (SPAWNS[k][1] - r.b.z);
                if (!netFoe(i)) { if (d > 9.0f) continue; d = 0.0f; }
                nearest = std::min(nearest, d);
            }
            if (nearest > best + 1e-3f) { best = nearest; sx = SPAWNS[k][0]; sz = SPAWNS[k][1]; }
        }
    }
    g_player = Player();
    g_player.x = sx;
    g_player.z = sz;
    g_player.face = std::atan2(-g_player.x, -g_player.z);   // de frente para o meio da sala
    g_camYaw = g_player.face + PI;                           // camera atras dele
    g_camPitch = 0.12f;                                 // quase na horizontal: a mira aponta para longe, nao para o chao
    g_camLift = 0.0f;
    g_camDist = CAM_DIST;
    g_proj.clear();
    botRespawn();
}

// Comeca a partida: zera o placar e prepara o oponente -- o bot (genero oposto) ou, na rede, os outros jogadores da
// sala (que aparecem no mapa quando chegar o estado de cada um).
static void startMatch() {
    ensureWorld();
    g_scorePlayer = g_scoreBot = 0;
    g_myKills = 0;
    g_lastHitBy = -1;
    g_bot = Bot();
    if (g_net != NET_NONE) {
        g_myMatch = g_matchId;
        g_proj.clear();
    } else {
        int opp = 1 - g_choice;
        g_bot.active = g_models[opp].loaded && g_models[g_choice].loaded;
        g_bot.model = opp;
        g_bot.look = g_look[opp];
        g_bot.look.team = 1;
        g_bot.look.outfit = true;                       // sempre de roupa tatica: o colete vermelho identifica o inimigo
        g_bot.name = "Bot";
    }
    if (!g_navBuilt) buildNav();
    g_hasGun = true;                                    // ja comeca armado: o bot nao espera
    g_armBlend = 1.0f;
    g_shoulder = SHOULDER_OFFSET;
    respawnPlayer();
}

// Leva a tela para onde a sala esta (o anfitriao decide, todos seguem): partida ou sala de espera.
static void netSyncState() {
    if (g_net == NET_NONE || (g_net == NET_JOIN && !g_connected)) return;
    if (g_phase != PH_LOBBY) {
        if (g_state != STATE_PLAY || g_myMatch != g_matchId) { g_state = STATE_PLAY; g_menu = MENU_NONE; startMatch(); }
    } else if (g_state != STATE_ROOM) {
        g_state = STATE_ROOM;
        g_menu = MENU_NONE;
        g_proj.clear();
        g_netStatus.clear();
    }
}

// input = false: o jogo continua (partida em rede com o menu aberto), mas o jogador nao controla nada.
static void update(float dt, float mouseDx, float mouseDy, bool input = true) {
    // camera
    const float sens = 0.0025f;
    g_camYaw   -= mouseDx * sens;
    g_camPitch += mouseDy * sens;
    if (input && g_keys[VK_LEFT])  g_camYaw += 2.0f * dt;
    if (input && g_keys[VK_RIGHT]) g_camYaw -= 2.0f * dt;
    if (input && g_keys[VK_UP])    g_camPitch -= 1.2f * dt;
    if (input && g_keys[VK_DOWN])  g_camPitch += 1.2f * dt;
    g_camPitch = clampf(g_camPitch, -0.1f, 1.4f);

    if (g_hurtTimer > 0.0f) g_hurtTimer -= dt;
    if (g_invuln > 0.0f) g_invuln -= dt;
    if (g_hitMarker > 0.0f) g_hitMarker -= dt;
    if (!g_keys[VK_LBUTTON]) g_fireLock = false;

    // eliminado: cai, espera e renasce
    if (g_playerDead) {
        g_playerFall = g_playerFall + dt / 0.6f > 1.0f ? 1.0f : g_playerFall + dt / 0.6f;
        g_deadTimer -= dt;
        if (g_deadTimer <= 0.0f) respawnPlayer();
    }
    const bool alive = !g_playerDead;
    const bool act = alive && input;                // pode controlar o personagem agora

    // item na mao: 1 = mao nua, 2 = arma
    if (act && pressed('1')) g_hasGun = false;
    if (act && pressed('2')) g_hasGun = true;
    g_armBlend = clampf(g_armBlend + (g_hasGun ? dt : -dt) / 0.3f, 0.0f, 1.0f);

    // direcoes relativas a camera, no plano XZ
    float fx = -std::sin(g_camYaw), fz = -std::cos(g_camYaw);
    float rx = -fz, rz = fx;

    float iy = act ? (g_keys['W'] ? 1.0f : 0.0f) - (g_keys['S'] ? 1.0f : 0.0f) : 0.0f;
    float ix = act ? (g_keys['D'] ? 1.0f : 0.0f) - (g_keys['A'] ? 1.0f : 0.0f) : 0.0f;

    float dx = fx * iy + rx * ix;
    float dz = fz * iy + rz * ix;
    float len = std::sqrt(dx * dx + dz * dz);

    Player& p = g_player;
    bool moving = len > 0.001f;
    float maxSpeed = g_keys[VK_SHIFT] ? RUN_SPEED : WALK_SPEED;
    if (moving) { dx /= len; dz /= len; }

    // atirando, ou parado com a arma na mao: o corpo (e a arma) viram para onde a camera mira
    bool firing = g_aimHold > 0.0f;
    bool aiming = alive && (firing || (g_hasGun && !moving));
    if (firing) g_aimHold -= dt;
    if (aiming) p.face += angleDiff(std::atan2(fx, fz), p.face) * (1.0f - std::exp(-(firing ? 18.0f : 9.0f) * dt));
    moveActor(p, dx, dz, moving, maxSpeed, dt, !aiming);

    // pulo e gravidade
    if (act && g_keys[VK_SPACE] && p.onGround) {
        p.vy = 9.0f * SCALE;
        p.onGround = false;
    }
    bool wasOnGround = p.onGround;
    p.vy -= 25.0f * SCALE * dt;
    float fallSpeed = p.vy;
    p.y += p.vy * dt;

    float gh = groundHeightAt(p.x, p.z, p.y);
    if (p.y <= gh && p.vy <= 0.0f) {
        p.y = gh;
        p.vy = 0.0f;
        p.onGround = true;
    } else {
        p.onGround = false;
    }
    if (!wasOnGround && p.onGround && fallSpeed < -4.0f * SCALE) p.land = 1.0f;   // pousou: os joelhos amortecem
    p.land = std::max(0.0f, p.land - dt / 0.32f);
    p.air += ((p.onGround ? 0.0f : 1.0f) - p.air) * (1.0f - std::exp(-16.0f * dt));
    if (g_recoil > 0.0f) g_recoil = std::max(0.0f, g_recoil - dt / 0.14f);
    fireBlendStep(g_shotHold, g_fireBlend, g_reloadTimer > 0.0f, dt);
    updateParticles(dt);

    // tiro: botao esquerdo do mouse, so com a arma ja levantada; segurando, atira em rajada
    if (g_fireCd > 0.0f) g_fireCd -= dt;
    if (g_flash > 0.0f) g_flash -= dt;
    if (g_reloadTimer > 0.0f) {                         // recarregando: nao atira ate acabar
        g_reloadTimer -= dt;
        if (g_reloadTimer <= 0.0f) { g_reloadTimer = 0.0f; g_ammo = MAG_SIZE; }
    }
    if (act && pressed('R') && g_hasGun && g_reloadTimer <= 0.0f && g_ammo < MAG_SIZE)   // recarga manual, antes de acabar
        startReload();
    updateCamera(p, dt);
    if (act && g_keys[VK_LBUTTON] && !g_fireLock && g_hasGun && g_armBlend >= 0.95f && g_fireCd <= 0.0f && g_reloadTimer <= 0.0f && g_ammo > 0) {
        fireProjectile(p);
        g_recoil = 1.0f; g_shotHold = SHOT_HOLD;
        botHearsPlayer();
        g_fireCd = FIRE_INTERVAL;
        g_flash = 0.06f;
        g_aimHold = 0.5f;
        if (--g_ammo == 0) startReload();
    }
    botUpdate(dt);
    remotesUpdate(dt);
    updateProjectiles(p, dt);
}

// ---------------------------------------------------------------- cenas

static const float SELECT_X[2] = { -1.4f * SCALE, 1.4f * SCALE };
static const float SELECT_CAM_Z = 5.2f;
static const float MENU_Z = 25.0f;          // as telas de escolha acontecem neste ponto do galpao (livre de caixas)

// Camera, cenario e luz das telas de escolha, nome e modo de jogo.
static void menuScene(Probe& pr) {
    setupView(0.0f, 1.7f * SCALE, MENU_Z + SELECT_CAM_Z, 0.0f, 1.05f, MENU_Z);
    drawWorld();
    lightProbe(0.0f, 0.0f, MENU_Z, pr);
    applyProbe(pr);
}

static void renderSelect(HDC hdc, const Font& big, const Font& mid, const Font& small, bool ui) {
    Probe pr;
    menuScene(pr);

    for (int i = 0; i < 2; ++i) {
        Model& m = g_models[i];
        if (!m.loaded) continue;
        bool sel = (i == g_choice);
        // o escolhido gira devagar andando no lugar; o outro espera de frente para a camera
        float face = sel ? g_time * 0.9f : 0.0f;
        poseModel(m, g_time * gaitCadence(WALK_SPEED / SCALE), sel ? WALK_SPEED / SCALE : 0.0f, g_time);

        drawShadow(SELECT_X[i], 0.0f, MENU_Z, pr.direct);
        glPushMatrix();
        glTranslatef(SELECT_X[i], 0.0f, MENU_Z);
        glRotatef(face * 180.0f / PI, 0, 1, 0);
        drawModel(m, sel ? 1.0f : 0.72f, g_look[i]);
        glPopMatrix();
    }
    drawLightGlow();

    if (!ui) return;                // menu aberto: so a cena, o menu desenha o resto

    // texto
    float aspect = (float)g_width / (float)(g_height > 0 ? g_height : 1);
    float halfW = SELECT_CAM_Z * std::tan(30.0f * PI / 180.0f) * aspect;   // meia largura visivel em z = 0
    begin2D();
    float cx = g_width * 0.5f;
    drawText(hdc, big, "ESCOLHA SEU PERSONAGEM", cx, g_height - 90.0f, 1.0f, 1.0f, 1.0f);
    const char* names[2] = { "HOMEM", "MULHER" };
    for (int i = 0; i < 2; ++i) {
        float sx = cx + (SELECT_X[i] / halfW) * cx;
        bool sel = (i == g_choice);
        drawText(hdc, mid, names[i], sx, g_height * 0.17f, sel ? 1.0f : 0.7f, sel ? 0.9f : 0.7f, sel ? 0.3f : 0.7f);
    }

    const Look& lk = g_look[g_choice];
    char line[160];
    if (g_choice == 0)
        snprintf(line, sizeof(line), "Cabelo: %s  (W / S)      Cor: %s  (Q / E)      Barba: %s  (B)      Roupa: %s  (T)",
                 HAIR_NAMES[lk.hair], COLOR_NAMES[lk.color], lk.beard ? "sim" : "nao", lk.outfit ? "t\xE1tica" : "nenhuma");
    else
        snprintf(line, sizeof(line), "Cabelo: %s  (W / S)      Cor: %s  (Q / E)      Roupa: %s  (T)",
                 HAIR_NAMES[lk.hair], COLOR_NAMES[lk.color], lk.outfit ? "t\xE1tica" : "nenhuma");
    drawText(hdc, small, line, cx, 62.0f, 1.0f, 0.95f, 0.6f);
    drawText(hdc, small, "A / D ou setas: trocar personagem     Enter: continuar     Esc: menu", cx, 30.0f, 1.0f, 1.0f, 1.0f);
    end2D();
}

static const float NAME_TAG_HEIGHT = PLAYER_HEIGHT + 0.45f;   // altura do nome acima dos pes

// Etapa 2 da criacao: o personagem escolhido fica no centro e o jogador digita o nome.
static void renderName(HDC hdc, const Font& big, const Font& small, const Font& tag) {
    Probe pr;
    menuScene(pr);

    Model& m = g_models[g_choice];
    if (m.loaded) {
        poseModel(m, 0.0f, 0.0f, g_time);
        drawShadow(0.0f, 0.0f, MENU_Z, pr.direct);
        glPushMatrix();
        glTranslatef(0.0f, 0.0f, MENU_Z);
        glRotatef(g_time * 35.0f, 0, 1, 0);
        drawModel(m, 1.0f, g_look[g_choice]);
        glPopMatrix();
    }
    drawLightGlow();
    drawText3D(hdc, tag, displayName().c_str(), 0.0f, NAME_TAG_HEIGHT, MENU_Z, 1.0f, 1.0f, 1.0f);

    begin2D();
    float cx = g_width * 0.5f;
    drawText(hdc, big, "COMO VOC\xCA SE CHAMA?", cx, g_height - 52.0f, 1.0f, 1.0f, 1.0f);

    // caixa de texto
    float bw = 520.0f, bh = 64.0f, by = 62.0f + 14.0f;
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glColor4f(0.0f, 0.0f, 0.0f, 0.45f);
    glBegin(GL_QUADS);
    glVertex2f(cx - bw * 0.5f, by); glVertex2f(cx + bw * 0.5f, by);
    glVertex2f(cx + bw * 0.5f, by + bh); glVertex2f(cx - bw * 0.5f, by + bh);
    glEnd();
    glDisable(GL_BLEND);
    glColor3f(1.0f, 0.9f, 0.3f);
    glBegin(GL_LINE_LOOP);
    glVertex2f(cx - bw * 0.5f, by); glVertex2f(cx + bw * 0.5f, by);
    glVertex2f(cx + bw * 0.5f, by + bh); glVertex2f(cx - bw * 0.5f, by + bh);
    glEnd();

    if (g_name.empty()) {
        drawText(hdc, big, "Jogador", cx, by + 14.0f, 0.5f, 0.5f, 0.5f);       // sugestao, some ao digitar
    } else {
        bool blink = std::fmod(g_time, 1.0f) < 0.5f;
        std::string shown = g_name + (blink && g_name.size() < MAX_NAME ? "_" : " ");
        drawText(hdc, big, shown.c_str(), cx, by + 14.0f, 1.0f, 1.0f, 1.0f);
    }

    drawText(hdc, small, "Digite seu nome (ate 16 letras)     Enter: continuar     Esc: voltar", cx, 30.0f, 1.0f, 1.0f, 1.0f);
    end2D();
}

// ---- telas de modo de jogo e de sala na rede
static int g_lobbySel = 0;
static const int NUM_LOBBY_ITEMS = 3;
static const char* const LOBBY_ITEMS[NUM_LOBBY_ITEMS] = { "Contra o bot", "Criar sala na rede", "Entrar em sala na rede" };
static std::string g_localIPs;

static void panel2D(float x0, float y0, float x1, float y1, float a) {
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glColor4f(0.0f, 0.0f, 0.0f, a);
    glBegin(GL_QUADS);
    glVertex2f(x0, y0); glVertex2f(x1, y0); glVertex2f(x1, y1); glVertex2f(x0, y1);
    glEnd();
    glDisable(GL_BLEND);
}

static const float TEAM_RGB[2][3] = { { 0.45f, 0.65f, 1.0f }, { 1.0f, 0.40f, 0.35f } };   // azul, vermelha

static void renderLobby(HDC hdc, const Font& big, const Font& mid, const Font& small, const Font& tag) {
    // fundo: a sala com o personagem escolhido girando, escurecida no meio para o texto
    Probe pr;
    menuScene(pr);
    Model& m = g_models[g_choice];
    if (m.loaded) {
        poseModel(m, 0.0f, 0.0f, g_time, 1.0f);
        drawShadow(0.0f, 0.0f, MENU_Z, pr.direct);
        glPushMatrix();
        glTranslatef(0.0f, 0.0f, MENU_Z);
        glRotatef(g_time * 35.0f, 0, 1, 0);
        Look lk = g_look[g_choice];
        if (g_state == STATE_ROOM && g_mode == MODE_TEAMS) { lk.outfit = true; lk.team = g_myTeam == 0 ? 2 : 1; }
        drawModel(m, 1.0f, lk);
        drawWeapon(m, 0.0f);
        glPopMatrix();
    }
    drawLightGlow();

    begin2D();
    float cx = g_width * 0.5f, cy = g_height * 0.5f;
    panel2D(cx - 400.0f, 70.0f, cx + 400.0f, g_height - 90.0f, 0.62f);

    bool blink = std::fmod(g_time, 1.0f) < 0.5f;
    char line[160];
    if (g_state == STATE_LOBBY) {
        drawText(hdc, big, "MODO DE JOGO", cx, g_height - 52.0f, 1.0f, 1.0f, 1.0f);
        for (int i = 0; i < NUM_LOBBY_ITEMS; ++i) {
            bool sel = i == g_lobbySel;
            snprintf(line, sizeof(line), sel ? "> %s <" : "%s", LOBBY_ITEMS[i]);
            drawText(hdc, mid, line, cx, cy + 50.0f - i * 60.0f, sel ? 1.0f : 0.75f, sel ? 0.9f : 0.75f, sel ? 0.3f : 0.75f);
        }
        snprintf(line, sizeof(line), "Mapa:   <   %s   >", MAP_NAMES[g_map]);
        drawText(hdc, tag, line, cx, cy - 124.0f, 1.0f, 0.9f, 0.3f);
        drawText(hdc, small, MAP_INFO[g_map], cx, cy - 152.0f, 0.8f, 0.8f, 0.8f);
        drawText(hdc, small, "Na rede: at\xE9 8 jogadores, todos na mesma rede (Wi-Fi ou cabo).", cx, cy - 200.0f, 0.75f, 0.75f, 0.75f);
        drawText(hdc, small, "W / S: escolher     A / D: trocar o mapa     Enter: confirmar     Esc: voltar", cx, 30.0f, 1.0f, 1.0f, 1.0f);
    } else if (g_state == STATE_ROOM) {
        bool host = g_net == NET_HOST;
        std::string hostName = host ? displayName() : playerName(0);
        drawText(hdc, big, ("SALA DE " + hostName).c_str(), cx, g_height - 52.0f, 1.0f, 1.0f, 1.0f);
        snprintf(line, sizeof(line), host ? "<   %s   >" : "Modo: %s", MODE_NAMES[g_mode & 1]);
        drawText(hdc, mid, line, cx, cy + 205.0f, 1.0f, 0.9f, 0.3f);
        snprintf(line, sizeof(line), g_mode == MODE_TEAMS ? "Duas equipes. Vence a primeira que somar %d abates." :
                                                            "Cada um por si. Vence quem chegar primeiro a %d abates.", killLimit());
        drawText(hdc, small, line, cx, cy + 172.0f, 0.8f, 0.8f, 0.8f);
        snprintf(line, sizeof(line), host ? "Mapa: %s   (M troca)" : "Mapa: %s", MAP_NAMES[g_map]);
        drawText(hdc, small, line, cx, cy + 146.0f, 1.0f, 0.9f, 0.3f);

        // quem esta na sala, pelo numero (o anfitriao e o 0)
        struct Entry { int id; std::string name; int team; };
        std::vector<Entry> list;
        for (int i = 0; i < MAX_PLAYERS; ++i) {
            if (i == g_myId) { Entry e = { i, displayName(), g_myTeam }; list.push_back(e); }
            else if (g_rp[i].present) { Entry e = { i, g_rp[i].name, g_rp[i].team }; list.push_back(e); }
        }
        auto entryLine = [&](const Entry& e) {
            std::string t = e.name;
            if (e.id == 0) t += "  (anfitri\xE3o)";
            if (e.id == g_myId) t += "  (voc\xEA)";
            return t;
        };
        for (int col = 0; col < (g_mode == MODE_TEAMS ? 2 : 1); ++col) {
            float x = g_mode == MODE_TEAMS ? cx + (col == 0 ? -190.0f : 190.0f) : cx;
            if (g_mode == MODE_TEAMS) drawText(hdc, tag, col == 0 ? "EQUIPE AZUL" : "EQUIPE VERMELHA", x, cy + 110.0f,
                                               TEAM_RGB[col][0], TEAM_RGB[col][1], TEAM_RGB[col][2]);
            else drawText(hdc, tag, "JOGADORES", x, cy + 110.0f, 1.0f, 1.0f, 1.0f);
            int row = 0;
            for (size_t k = 0; k < list.size(); ++k) {
                if (g_mode == MODE_TEAMS && list[k].team != col) continue;
                bool me = list[k].id == g_myId;
                drawText(hdc, small, entryLine(list[k]).c_str(), x, cy + 76.0f - row * 30.0f, 1.0f, me ? 0.9f : 1.0f, me ? 0.3f : 1.0f);
                ++row;
            }
        }
        snprintf(line, sizeof(line), "%d / %d jogadores", (int)list.size(), MAX_PLAYERS);
        drawText(hdc, small, line, cx, cy - 165.0f, 0.8f, 0.8f, 0.8f);
        if (host) {
            drawText(hdc, small, "Nos outros PCs: \"Entrar em sala na rede\" (ou digite um destes IPs):", cx, cy - 200.0f, 1.0f, 1.0f, 1.0f);
            drawText(hdc, tag, g_localIPs.c_str(), cx, cy - 236.0f, 1.0f, 0.9f, 0.3f);
            drawText(hdc, small, g_mode == MODE_TEAMS ? "A / D: modo     M: mapa     T: equipe     Enter: come\xE7" "ar     Esc: fechar a sala"
                                                      : "A / D: modo     M: mapa     Enter: come\xE7" "ar a partida     Esc: fechar a sala",
                     cx, 30.0f, 1.0f, 1.0f, 1.0f);
        } else {
            drawText(hdc, mid, blink ? "Aguardando o anfitri\xE3o come\xE7" "ar..." : "Aguardando o anfitri\xE3o come\xE7" "ar",
                     cx, cy - 215.0f, 1.0f, 0.9f, 0.3f);
            drawText(hdc, small, g_mode == MODE_TEAMS ? "T: trocar de equipe     Esc: sair da sala" : "Esc: sair da sala",
                     cx, 30.0f, 1.0f, 1.0f, 1.0f);
        }
    } else {
        drawText(hdc, big, "ENTRAR EM SALA", cx, g_height - 52.0f, 1.0f, 1.0f, 1.0f);
        drawText(hdc, small, "Salas encontradas na rede:", cx, cy + 150.0f, 1.0f, 1.0f, 1.0f);
        if (g_found.empty()) {
            drawText(hdc, mid, blink ? "Procurando..." : "Procurando", cx, cy + 100.0f, 0.7f, 0.7f, 0.7f);
        } else {
            for (size_t i = 0; i < g_found.size() && i < 3; ++i) {
                const FoundHost& fh = g_found[i];
                bool sel = (int)i == g_foundSel && g_ipText.empty();
                snprintf(line, sizeof(line), sel ? "> %s  (%s) <" : "%s  (%s)", fh.name, inet_ntoa(fh.addr.sin_addr));
                float y = cy + 100.0f - (float)i * 70.0f;
                drawText(hdc, mid, line, cx, y, sel ? 1.0f : 0.75f, sel ? 0.9f : 0.75f, sel ? 0.3f : 0.75f);
                snprintf(line, sizeof(line), "%d / %d jogadores   -   %s   -   %s%s", fh.players, MAX_PLAYERS, MODE_NAMES[fh.mode & 1],
                         MAP_NAMES[fh.map % NUM_MAPS],
                         fh.players >= MAX_PLAYERS ? "   -   cheia" : (fh.phase != PH_LOBBY ? "   -   em jogo (d\xE1 para entrar)" : ""));
                drawText(hdc, small, line, cx, y - 28.0f, 0.8f, 0.8f, 0.8f);
            }
        }
        drawText(hdc, small, "Ou digite o IP do anfitri\xE3o:", cx, cy - 140.0f, 1.0f, 1.0f, 1.0f);
        std::string shown = g_ipText.empty() ? (blink ? "_" : " ") : g_ipText + (blink ? "_" : " ");
        drawText(hdc, tag, shown.c_str(), cx, cy - 180.0f, 1.0f, 0.9f, 0.3f);
        drawText(hdc, small, "W / S: escolher     Enter: entrar     Esc: voltar", cx, 30.0f, 1.0f, 1.0f, 1.0f);
    }
    if (!g_netStatus.empty())
        drawText(hdc, small, g_netStatus.c_str(), cx, 84.0f, 1.0f, 0.45f, 0.4f);
    end2D();
}

// ---- placar da partida em rede
struct ScoreRow { int id, team, kills, deaths; std::string name; };

// Todos os jogadores da sala, do que tem mais abates para o que tem menos (empate: menos mortes primeiro).
static void netScoreRows(std::vector<ScoreRow>& rows) {
    rows.clear();
    ScoreRow me = { g_myId, g_myTeam, g_myKills, g_scoreBot, displayName() };
    rows.push_back(me);
    for (int i = 0; i < MAX_PLAYERS; ++i) {
        const Bot& r = g_rp[i];
        if (i == g_myId || !r.present) continue;
        ScoreRow row = { i, r.team, r.kills, r.deaths, r.name };
        rows.push_back(row);
    }
    std::stable_sort(rows.begin(), rows.end(), [](const ScoreRow& a, const ScoreRow& b) {
        return a.kills != b.kills ? a.kills > b.kills : a.deaths < b.deaths;
    });
}

// Tabela no meio da tela (Tab segurado, ou no fim da partida com o vencedor).
static void drawScoreboard(HDC hdc, const Font& small, const Font& tag) {
    std::vector<ScoreRow> rows;
    netScoreRows(rows);
    bool teams = g_mode == MODE_TEAMS;
    float cx = g_width * 0.5f, cy = g_height * 0.5f, rowH = 30.0f;
    float h = 140.0f + rows.size() * rowH, top = cy + h * 0.5f;
    panel2D(cx - 330.0f, top - h, cx + 330.0f, top, 0.75f);
    char line[128];
    float tr = 1.0f, tg = 1.0f, tb = 1.0f;
    if (g_phase == PH_END && g_winner >= 0) {
        if (teams) snprintf(line, sizeof(line), "FIM  -  EQUIPE %s VENCEU", g_winner == 0 ? "AZUL" : "VERMELHA");
        else snprintf(line, sizeof(line), "FIM  -  %s VENCEU", playerName(g_winner).c_str());
        tr = 1.0f; tg = 0.85f; tb = 0.3f;
    } else if (teams) {
        int tk[2];
        netTeamKills(tk);
        snprintf(line, sizeof(line), "AZUL  %d   x   %d  VERMELHA", tk[0], tk[1]);
    } else {
        snprintf(line, sizeof(line), "PLACAR  (meta: %d abates)", killLimit());
    }
    drawText(hdc, tag, line, cx, top - 44.0f, tr, tg, tb);
    const float colName = cx - 160.0f, colTeam = cx + 70.0f, colK = cx + 165.0f, colD = cx + 255.0f;
    float y = top - 84.0f;
    drawText(hdc, small, "Jogador", colName, y, 0.7f, 0.7f, 0.7f);
    if (teams) drawText(hdc, small, "Equipe", colTeam, y, 0.7f, 0.7f, 0.7f);
    drawText(hdc, small, "Abates", colK, y, 0.7f, 0.7f, 0.7f);
    drawText(hdc, small, "Mortes", colD, y, 0.7f, 0.7f, 0.7f);
    for (size_t i = 0; i < rows.size(); ++i) {
        const ScoreRow& r = rows[i];
        y -= rowH;
        bool me = r.id == g_myId;
        float c[3] = { 1.0f, 1.0f, 1.0f };
        if (teams) { c[0] = TEAM_RGB[r.team & 1][0]; c[1] = TEAM_RGB[r.team & 1][1]; c[2] = TEAM_RGB[r.team & 1][2]; }
        if (me) { c[0] = 1.0f; c[1] = 0.9f; c[2] = 0.3f; }
        snprintf(line, sizeof(line), me ? "> %s" : "%s", r.name.c_str());
        drawText(hdc, small, line, colName, y, c[0], c[1], c[2]);
        if (teams) drawText(hdc, small, r.team == 0 ? "Azul" : "Vermelha", colTeam, y, c[0], c[1], c[2]);
        snprintf(line, sizeof(line), "%d", r.kills);
        drawText(hdc, small, line, colK, y, c[0], c[1], c[2]);
        snprintf(line, sizeof(line), "%d", r.deaths);
        drawText(hdc, small, line, colD, y, c[0], c[1], c[2]);
    }
    if (g_phase == PH_END) drawText(hdc, small, "Voltando para a sala...", cx, top - h + 18.0f, 0.8f, 0.8f, 0.8f);
}

// Placar resumido no alto da tela e as ultimas eliminacoes / entradas / saidas no canto superior direito.
static void drawNetHud(HDC hdc, const Font& small, const Font& tag) {
    float cx = g_width * 0.5f, top = g_height - 48.0f;
    char buf[128], sub[128];
    if (g_mode == MODE_TEAMS) {
        int tk[2];
        netTeamKills(tk);
        snprintf(buf, sizeof(buf), "AZUL  %d", tk[0]);
        drawText(hdc, tag, buf, cx - 95.0f, top, TEAM_RGB[0][0], TEAM_RGB[0][1], TEAM_RGB[0][2]);
        drawText(hdc, tag, "x", cx, top, 1.0f, 1.0f, 1.0f);
        snprintf(buf, sizeof(buf), "%d  VERMELHA", tk[1]);
        drawText(hdc, tag, buf, cx + 115.0f, top, TEAM_RGB[1][0], TEAM_RGB[1][1], TEAM_RGB[1][2]);
        snprintf(sub, sizeof(sub), "Sua equipe: %s     Seus abates: %d     Meta: %d", g_myTeam == 0 ? "Azul" : "Vermelha",
                 g_myKills, killLimit());
    } else {
        std::vector<ScoreRow> rows;
        netScoreRows(rows);
        int place = 1;
        for (size_t i = 0; i < rows.size() && rows[i].id != g_myId; ++i) ++place;
        snprintf(buf, sizeof(buf), "Voc\xEA: %d abates   (%d\xBA de %d)", g_myKills, place, (int)rows.size());
        drawText(hdc, tag, buf, cx, top, 1.0f, 1.0f, 1.0f);
        snprintf(sub, sizeof(sub), "Na frente: %s (%d)     Meta: %d abates", rows[0].name.c_str(), rows[0].kills, killLimit());
    }
    drawText(hdc, small, sub, cx, top - 28.0f, 0.8f, 0.8f, 0.8f);

    float y = g_height - 40.0f;
    for (int i = (int)g_feed.size() - 1; i >= 0; --i) {
        const FeedLine& l = g_feed[(size_t)i];
        float age = g_time - l.t;
        if (age > 6.0f || age < 0.0f) continue;
        float a = age > 5.0f ? 6.0f - age : 1.0f;
        float w = textWidth(small, l.text.c_str());
        float r = 1.0f, g = 1.0f, b = 1.0f;
        if (l.kind == 1) { g = 0.85f; b = 0.3f; }
        if (l.kind == 2) { g = 0.4f; b = 0.35f; }
        drawText(hdc, small, l.text.c_str(), g_width - 24.0f - w * 0.5f, y, r, g, b, a);
        y -= 26.0f;
    }
}

// Coracao centrado em (cx, cy); s = tamanho (largura total ~ 1,9 s). Preenchimento com transparencia a.
static void drawHeart(float cx, float cy, float s, float r, float g, float b, float a, float lr, float lg, float lb) {
    const int N = 40;
    float px[N], py[N];
    for (int i = 0; i < N; ++i) {
        float t = 2.0f * PI * i / N;
        float st = std::sin(t);
        px[i] = cx + 16.0f * st * st * st * s / 17.0f;
        py[i] = cy + (13.0f * std::cos(t) - 5.0f * std::cos(2 * t) - 2.0f * std::cos(3 * t) - std::cos(4 * t) + 2.0f) * s / 17.0f;
    }
    glColor4f(r, g, b, a);
    glBegin(GL_TRIANGLE_FAN);
    glVertex2f(cx, cy + 3.0f * s / 17.0f);
    for (int i = 0; i <= N; ++i) glVertex2f(px[i % N], py[i % N]);
    glEnd();
    glColor4f(lr, lg, lb, a > 0.3f ? 1.0f : 0.8f);
    glLineWidth(2.0f);
    glBegin(GL_LINE_LOOP);
    for (int i = 0; i < N; ++i) glVertex2f(px[i], py[i]);
    glEnd();
}

static void drawHearts() {
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glEnable(GL_LINE_SMOOTH);
    for (int i = 0; i < MAX_HP; ++i) {
        float x = 40.0f + i * 44.0f, y = g_height - 40.0f;
        if (i < g_hp) {
            drawHeart(x, y, 17.0f, 0.88f, 0.12f, 0.20f, 1.0f, 0.25f, 0.0f, 0.03f);
        } else {
            drawHeart(x, y, 17.0f, 0.0f, 0.0f, 0.0f, 0.35f, 0.75f, 0.75f, 0.75f);          // perdido: vazio
            if (i == g_hp && g_hurtTimer > 0.0f) {                                          // acabou de ser perdido: estoura e some
                float k = g_hurtTimer / HURT_TIME;
                drawHeart(x, y, 17.0f * (1.0f + (1.0f - k) * 0.8f), 1.0f, 0.2f, 0.2f, k, 1.0f, 0.5f, 0.5f);
            }
        }
    }
    glDisable(GL_LINE_SMOOTH);
    glLineWidth(1.0f);
    glDisable(GL_BLEND);
}

// Escurece (ou tinge) as bordas da tela: quatro faixas que vao da cor, na borda, ate transparente, no meio.
static void drawVignette(float r, float g, float b, float a) {
    float w = (float)g_width, h = (float)g_height, ix = w * 0.26f, iy = h * 0.30f;
    const float ox[4] = { 0, w, w, 0 }, oy[4] = { 0, 0, h, h }, nx[4] = { ix, w - ix, w - ix, ix }, ny[4] = { iy, iy, h - iy, h - iy };
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glBegin(GL_QUADS);
    for (int k = 0; k < 4; ++k) {
        int n = (k + 1) % 4;
        glColor4f(r, g, b, a); glVertex2f(ox[k], oy[k]); glVertex2f(ox[n], oy[n]);
        glColor4f(r, g, b, 0.0f); glVertex2f(nx[n], ny[n]); glVertex2f(nx[k], ny[k]);
    }
    glEnd();
    glDisable(GL_BLEND);
}

// Clarao do tiro batendo no chao perto da arma (chamar logo depois de desenhar o personagem).
static void drawFlashGlow(const Player& c, int modelIdx, float flash) {
    if (flash <= 0.0f) return;
    float mz[3];
    muzzleWorld(c, modelIdx, mz);
    float k = clampf(flash / 0.06f, 0.0f, 1.0f), r = 2.6f, y = c.y + 0.05f;
    glPushAttrib(GL_ENABLE_BIT | GL_DEPTH_BUFFER_BIT | GL_COLOR_BUFFER_BIT);
    glDisable(GL_LIGHTING);
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, g_tex[TX_GLOW]);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE);
    glDepthMask(GL_FALSE);
    glColor4f(1.0f, 0.78f, 0.42f, 0.28f * k);
    glBegin(GL_QUADS);
    glTexCoord2f(0, 0); glVertex3f(mz[0] - r, y, mz[2] - r);
    glTexCoord2f(1, 0); glVertex3f(mz[0] + r, y, mz[2] - r);
    glTexCoord2f(1, 1); glVertex3f(mz[0] + r, y, mz[2] + r);
    glTexCoord2f(0, 1); glVertex3f(mz[0] - r, y, mz[2] + r);
    glEnd();
    glPopAttrib();
}

static void renderPlay(HDC hdc, const Font& small, const Font& tag, bool ui) {
    const Player& p = g_player;
    setupOrbitCamera(p);

    drawWorld();
    drawDecals();
    // caido, a sombra acompanha o corpo deitado para tras
    Probe pr;
    lightProbe(p.x, p.y, p.z, pr);
    float pk = 1.5f * smooth01(g_playerFall);
    drawShadow(p.x - std::sin(p.face) * pk, groundHeightAt(p.x, p.z, p.y), p.z - std::cos(p.face) * pk, pr.direct);
    applyProbe(pr);
    drawPlayer(p);
    drawFlashGlow(p, g_choice, g_armBlend > 0.9f ? g_flash : 0.0f);
    float eye[3], cf[3], cs[3], cu[3];
    cameraRig(p, eye, cf, cs, cu);
    // o bot ou os outros jogadores da rede
    auto drawOther = [&](const Bot& o, const Look& lk, float armed, float seed) {
        float bk = 1.5f * smooth01(o.fall);
        lightProbe(o.b.x, o.b.y, o.b.z, pr);
        drawShadow(o.b.x - std::sin(o.b.face) * bk, groundHeightAt(o.b.x, o.b.z, o.b.y), o.b.z - std::cos(o.b.face) * bk, pr.direct);
        applyProbe(pr);
        PoseFx fx;
        fx.aimPitch = o.aimPitch;
        fx.recoil = o.recoil;
        fx.fire = smooth01(o.fireBlend);
        fx.reload = o.remote ? o.reloadProg
                             : (o.reloadTimer > 0.0f && o.reloadTotal > 0.0f ? 1.0f - o.reloadTimer / o.reloadTotal : -1.0f);
        fx.air = o.b.air;
        fx.land = o.b.land;
        fx.seed = seed;
        drawCharacter(o.b, o.model, lk, armed, o.flash, o.hurt > 0.0f ? o.hurt : 0.0f, o.fall, fx);
        drawFlashGlow(o.b, o.model, o.flash);
    };
    const Bot& bt = g_bot;
    if (bt.active) drawOther(bt, bt.look, 1.0f, 7.3f);
    for (int i = 0; i < MAX_PLAYERS; ++i) {
        const Bot& r = g_rp[i];
        if (i == g_myId || !r.present || !r.active) continue;
        if ((r.b.x - eye[0]) * cf[0] + (r.b.z - eye[2]) * cf[2] < -4.0f) continue;   // atras da camera: nem posa
        Look lk = r.look;
        lk.outfit = true;
        lk.team = g_mode == MODE_TEAMS ? (r.team == 0 ? 2 : 1) : 1;
        drawOther(r, lk, smooth01(r.arm), 1.7f * (float)i);
    }
    // projeteis: o cubinho brilhante (amarelo do jogador, vermelho do adversario) e um rastro de luz atras dele
    float mv[16];
    glGetFloatv(GL_MODELVIEW_MATRIX, mv);
    const float camF[3] = { -mv[2], -mv[6], -mv[10] };
    glDisable(GL_LIGHTING);
    // tiro de companheiro de equipe: azul
    auto friendlyShot = [](int owner) { return owner >= 2 && g_mode == MODE_TEAMS && g_rp[owner - 2].team == g_myTeam; };
    for (size_t i = 0; i < g_proj.size(); ++i) {
        if (g_proj[i].owner == 0) glColor3f(1.0f, 0.86f, 0.30f);
        else if (friendlyShot(g_proj[i].owner)) glColor3f(0.35f, 0.60f, 1.0f);
        else glColor3f(1.0f, 0.30f, 0.22f);
        glPushMatrix();
        glTranslatef(g_proj[i].p[0], g_proj[i].p[1], g_proj[i].p[2]);
        drawBox(BULLET_SIZE, BULLET_SIZE, BULLET_SIZE);
        glPopMatrix();
    }
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE);
    glDepthMask(GL_FALSE);
    glBegin(GL_QUADS);
    for (size_t i = 0; i < g_proj.size(); ++i) {
        const Projectile& b = g_proj[i];
        float d[3] = { b.v[0], b.v[1], b.v[2] };
        norm3(d);
        float sd[3] = { d[1] * camF[2] - d[2] * camF[1], d[2] * camF[0] - d[0] * camF[2], d[0] * camF[1] - d[1] * camF[0] };
        norm3(sd);
        float len = std::min(b.dist, 3.0f), w = 0.055f;
        float c[3] = { 1.0f, b.owner == 0 ? 0.80f : 0.28f, b.owner == 0 ? 0.30f : 0.20f };
        if (friendlyShot(b.owner)) { c[0] = 0.35f; c[1] = 0.60f; c[2] = 1.0f; }
        glColor4f(c[0], c[1], c[2], 0.80f);
        glVertex3f(b.p[0] + sd[0] * w, b.p[1] + sd[1] * w, b.p[2] + sd[2] * w);
        glVertex3f(b.p[0] - sd[0] * w, b.p[1] - sd[1] * w, b.p[2] - sd[2] * w);
        glColor4f(c[0], c[1], c[2], 0.0f);
        glVertex3f(b.p[0] - d[0] * len - sd[0] * w, b.p[1] - d[1] * len - sd[1] * w, b.p[2] - d[2] * len - sd[2] * w);
        glVertex3f(b.p[0] - d[0] * len + sd[0] * w, b.p[1] - d[1] * len + sd[1] * w, b.p[2] - d[2] * len + sd[2] * w);
    }
    glEnd();
    glDepthMask(GL_TRUE);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_BLEND);
    glEnable(GL_LIGHTING);
    drawParticles();
    drawLightGlow();

    if (!ui) return;
    if (!g_playerDead) drawText3D(hdc, tag, displayName().c_str(), p.x, p.y + NAME_TAG_HEIGHT, p.z, 1.0f, 1.0f, 1.0f);
    if (bt.active && !bt.dead) drawText3D(hdc, tag, bt.name.c_str(), bt.b.x, bt.b.y + NAME_TAG_HEIGHT, bt.b.z, 1.0f, 0.4f, 0.35f);
    for (int i = 0; i < MAX_PLAYERS; ++i) {             // nomes: companheiros em azul, inimigos em vermelho
        const Bot& r = g_rp[i];
        if (i == g_myId || !r.present || !r.active || r.dead) continue;
        bool foe = netFoe(i);
        drawText3D(hdc, tag, r.name.c_str(), r.b.x, r.b.y + NAME_TAG_HEIGHT, r.b.z, foe ? 1.0f : 0.5f, foe ? 0.4f : 0.75f, foe ? 0.35f : 1.0f);
    }

    begin2D();
    drawVignette(0.0f, 0.0f, 0.0f, g_worldMap == MAP_BRANCO ? 0.16f : 0.42f);    // bordas da tela levemente escuras
    if (g_hp <= 2 && !g_playerDead)                    // pouca vida: bordas vermelhas pulsando
        drawVignette(0.7f, 0.0f, 0.0f, (g_hp == 1 ? 0.34f : 0.20f) * (0.75f + 0.25f * std::sin(g_time * 5.0f)));
    if (g_hurtTimer > 0.0f)                            // levou um tiro: bordas vermelhas por um instante
        drawVignette(0.85f, 0.0f, 0.0f, 0.42f * g_hurtTimer / HURT_TIME);
    drawHearts();
    if (bt.active) {                                   // vida do bot (canto superior direito) e placar
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glEnable(GL_LINE_SMOOTH);
        for (int i = 0; i < MAX_HP; ++i) {
            float x = g_width - 40.0f - i * 44.0f, y = g_height - 40.0f;
            if (i < bt.hp) drawHeart(x, y, 17.0f, 0.88f, 0.12f, 0.20f, 1.0f, 0.25f, 0.0f, 0.03f);
            else           drawHeart(x, y, 17.0f, 0.0f, 0.0f, 0.0f, 0.35f, 0.75f, 0.75f, 0.75f);
        }
        glDisable(GL_LINE_SMOOTH);
        glLineWidth(1.0f);
        glDisable(GL_BLEND);
        drawText(hdc, small, bt.name.c_str(), g_width - 40.0f - (MAX_HP - 1) * 44.0f * 0.5f, g_height - 84.0f, 1.0f, 0.5f, 0.45f);
        char sc[64];
        snprintf(sc, sizeof(sc), "Voc\xEA  %d  x  %d  %s", g_scorePlayer, g_scoreBot, bt.name.c_str());
        drawText(hdc, tag, sc, g_width * 0.5f, g_height - 48.0f, 1.0f, 1.0f, 1.0f);
    }
    if (g_net != NET_NONE) drawNetHud(hdc, small, tag);
    if (g_playerDead) {
        drawText(hdc, tag, "VOC\xCA FOI ELIMINADO", g_width * 0.5f, g_height * 0.5f + 60.0f, 1.0f, 0.3f, 0.3f);
        char rb[64];
        float ry = g_height * 0.5f + 24.0f;
        if (g_net != NET_NONE && g_lastHitBy >= 0) {
            snprintf(rb, sizeof(rb), "por %s", playerName(g_lastHitBy).c_str());
            drawText(hdc, small, rb, g_width * 0.5f, ry, 1.0f, 0.75f, 0.7f);
            ry -= 30.0f;
        }
        snprintf(rb, sizeof(rb), "Renascendo em %.0f...", g_deadTimer + 0.5f);
        drawText(hdc, small, rb, g_width * 0.5f, ry, 1.0f, 1.0f, 1.0f);
    }
    if (g_hurtTimer > 0.0f && !g_playerDead) {         // seta vermelha indicando de onde veio o tiro
        float hx = g_hurtDir[0], hz = g_hurtDir[1];
        float fl = std::sqrt(cf[0] * cf[0] + cf[2] * cf[2]);
        float side = hx * cs[0] + hz * cs[2];
        float front = fl > 1e-4f ? (hx * cf[0] + hz * cf[2]) / fl : 0.0f;
        float ang = std::atan2(side, front), sa = std::sin(ang), ca = std::cos(ang);
        float mx = g_width * 0.5f + sa * 170.0f, my = g_height * 0.5f + ca * 170.0f;
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glColor4f(1.0f, 0.15f, 0.1f, 0.9f * g_hurtTimer / HURT_TIME);
        glBegin(GL_TRIANGLES);
        glVertex2f(mx + sa * 26.0f, my + ca * 26.0f);
        glVertex2f(mx + ca * 20.0f, my - sa * 20.0f);
        glVertex2f(mx - ca * 20.0f, my + sa * 20.0f);
        glEnd();
        glDisable(GL_BLEND);
    }
    if (g_armBlend > 0.5f && !g_playerDead) {          // mira no centro da tela
        float mx = g_width * 0.5f, my = g_height * 0.5f;
        for (int pass = 0; pass < 2; ++pass) {
            glLineWidth(pass == 0 ? 4.0f : 2.0f);
            if (pass == 0) glColor3f(0.0f, 0.0f, 0.0f); else glColor3f(1.0f, 1.0f, 1.0f);
            glBegin(GL_LINES);
            glVertex2f(mx - 14.0f, my); glVertex2f(mx - 5.0f, my);
            glVertex2f(mx + 5.0f, my);  glVertex2f(mx + 14.0f, my);
            glVertex2f(mx, my - 14.0f); glVertex2f(mx, my - 5.0f);
            glVertex2f(mx, my + 5.0f);  glVertex2f(mx, my + 14.0f);
            glEnd();
        }
        if (g_hitMarker > 0.0f) {                      // acertou o bot: um X vermelho sobre a mira
            glLineWidth(3.0f);
            glColor3f(1.0f, 0.2f, 0.15f);
            glBegin(GL_LINES);
            glVertex2f(mx - 18.0f, my - 18.0f); glVertex2f(mx - 8.0f, my - 8.0f);
            glVertex2f(mx + 18.0f, my + 18.0f); glVertex2f(mx + 8.0f, my + 8.0f);
            glVertex2f(mx - 18.0f, my + 18.0f); glVertex2f(mx - 8.0f, my + 8.0f);
            glVertex2f(mx + 18.0f, my - 18.0f); glVertex2f(mx + 8.0f, my - 8.0f);
            glEnd();
        }
        glLineWidth(1.0f);
    }
    if (g_hasGun || g_reloadTimer > 0.0f) {            // municao no canto inferior direito
        bool reloading = g_reloadTimer > 0.0f;
        float ax = g_width - 110.0f;
        char buf[48];
        snprintf(buf, sizeof(buf), "%d / %d", g_ammo, MAG_SIZE);
        if (reloading)             drawText(hdc, tag, buf, ax, 66.0f, 1.0f, 0.55f, 0.2f);
        else if (g_ammo <= 5)      drawText(hdc, tag, buf, ax, 66.0f, 1.0f, 0.3f, 0.3f);
        else                       drawText(hdc, tag, buf, ax, 66.0f, 1.0f, 1.0f, 1.0f);
        if (reloading) {
            snprintf(buf, sizeof(buf), "Recarregando... %.1f s", g_reloadTimer);
            drawText(hdc, small, buf, ax, 40.0f, 1.0f, 0.85f, 0.6f);
            float bw = 170.0f, bx = ax - bw * 0.5f, by = 24.0f, k = 1.0f - g_reloadTimer / g_reloadTotal;
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            glColor4f(0.0f, 0.0f, 0.0f, 0.5f);
            glBegin(GL_QUADS);
            glVertex2f(bx, by); glVertex2f(bx + bw, by); glVertex2f(bx + bw, by + 8.0f); glVertex2f(bx, by + 8.0f);
            glEnd();
            glColor4f(1.0f, 0.65f, 0.2f, 1.0f);
            glBegin(GL_QUADS);
            glVertex2f(bx, by); glVertex2f(bx + bw * k, by); glVertex2f(bx + bw * k, by + 8.0f); glVertex2f(bx, by + 8.0f);
            glEnd();
            glDisable(GL_BLEND);
        }
    }
    float cx = g_width * 0.5f;
    drawText(hdc, small, "[1] M\xE3o nua", cx - 80.0f, 56.0f, g_hasGun ? 0.7f : 1.0f, g_hasGun ? 0.7f : 0.9f, g_hasGun ? 0.7f : 0.3f);
    drawText(hdc, small, "[2] Arma",     cx + 80.0f, 56.0f, g_hasGun ? 1.0f : 0.7f, g_hasGun ? 0.9f : 0.7f, g_hasGun ? 0.3f : 0.7f);
    drawText(hdc, small, g_net != NET_NONE ? "Esc: menu     Tab: placar" : "Esc: menu", cx, 24.0f, 1.0f, 1.0f, 1.0f);
    if (g_net != NET_NONE && (g_keys[VK_TAB] || g_phase == PH_END)) drawScoreboard(hdc, small, tag);
    end2D();
}

static float menuItemY(int i) { return g_height * 0.5f + 20.0f - i * 56.0f; }

// Opcoes do menu: durante a partida tem tambem como sair dela sem fechar o jogo.
static int menuCount() { return g_state == STATE_PLAY ? 4 : 3; }
static const char* menuItem(int i) {
    if (i == 0) return "Continuar";
    if (i == 1) return "Controles";
    if (i == 2 && menuCount() == 4)
        return g_net == NET_HOST ? "Encerrar e voltar \xE0 sala" : (g_net == NET_JOIN ? "Sair da sala" : "Voltar ao modo de jogo");
    return "Sair do jogo";
}

// Menu por cima da cena: escurece a tela e lista as opcoes (ou os controles).
static void renderMenu(HDC hdc, const Font& big, const Font& mid, const Font& small) {
    begin2D();
    float cx = g_width * 0.5f, cy = g_height * 0.5f;

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glColor4f(0.0f, 0.0f, 0.0f, 0.72f);
    glBegin(GL_QUADS);
    glVertex2f(0, 0); glVertex2f((float)g_width, 0);
    glVertex2f((float)g_width, (float)g_height); glVertex2f(0, (float)g_height);
    glEnd();
    glDisable(GL_BLEND);

    if (g_menu == MENU_MAIN) {
        drawText(hdc, big, "MENU", cx, cy + 110.0f, 1.0f, 1.0f, 1.0f);
        for (int i = 0; i < menuCount(); ++i) {
            bool sel = (i == g_menuSel);
            char line[64];
            snprintf(line, sizeof(line), sel ? "> %s <" : "%s", menuItem(i));
            drawText(hdc, mid, line, cx, menuItemY(i), sel ? 1.0f : 0.75f, sel ? 0.9f : 0.75f, sel ? 0.3f : 0.75f);
        }
        drawText(hdc, small, "Mouse ou W / S: escolher     Clique ou Enter: confirmar     Esc: fechar", cx, cy - 215.0f, 0.8f, 0.8f, 0.8f);
    } else {
        drawText(hdc, big, "CONTROLES", cx, cy + 150.0f, 1.0f, 1.0f, 1.0f);
        const char* lines[10];
        int n = 0;
        if (g_state == STATE_PLAY) {
            lines[n++] = "W A S D   -   andar";
            lines[n++] = "Mouse   -   girar a c\xE2mera";
            lines[n++] = "Shift   -   correr";
            lines[n++] = "Espa\xE7o   -   pular";
            lines[n++] = "1 / 2   -   m\xE3o nua / arma";
            lines[n++] = "Bot\xE3o esquerdo   -   atirar (com a arma)";
            lines[n++] = "R   -   recarregar";
            if (g_net != NET_NONE) lines[n++] = "Tab (segurar)   -   placar da sala";
            lines[n++] = "Esc   -   abrir / fechar o menu";
        } else {
            lines[n++] = "A / D ou setas   -   trocar personagem";
            lines[n++] = "W / S ou setas   -   estilo do cabelo";
            lines[n++] = "Q / E   -   cor do cabelo";
            lines[n++] = "B   -   barba (homem)";
            lines[n++] = "T   -   roupa t\xE1tica / nenhuma";
            lines[n++] = "Enter   -   continuar";
            lines[n++] = "Esc   -   abrir / fechar o menu";
        }
        for (int i = 0; i < n; ++i)
            drawText(hdc, mid, lines[i], cx, cy + 80.0f - i * 37.0f, 1.0f, 1.0f, 1.0f);
        drawText(hdc, small, "Clique, Enter ou Esc: voltar", cx, cy - 270.0f, 0.8f, 0.8f, 0.8f);
    }
    end2D();
}

// ---------------------------------------------------------------- teste de animacao (Tiritos.exe --anim-test <prefixo>)
// Mede o ciclo de andar/correr (deslize dos pes, penetracao no chao, suavidade) e gera uma imagem
// com 8 quadros do ciclo vistos de lado, para conferir a pose a olho.

typedef void (*PoseFn)(Model&, float cycles, float speed);   // cycles: fase em ciclos; speed: m/s do modelo

struct GaitCase { const char* label; PoseFn fn; float speed, cadence; };

static float angleOf(const Mat4& a, const Mat4& b) {          // direcao a->b no plano YZ (0 = para baixo, + = para tras)
    return std::atan2(-(b.m[14] - a.m[14]), -(b.m[13] - a.m[13]));
}

static void analyzeGait(FILE* out, Model& m, const GaitCase& gc) {
    const int N = 120;
    const float dt = 1.0f / (gc.cadence * N);
    struct Frame { float ay[2], by[2], gzA[2], gzB[2], hip, th[2], ca[2], ft[2], ar[2], fa[2]; };
    std::vector<Frame> fr(N);
    float restA[2], restB[2];
    for (int s = 0; s < 2; ++s) { restA[s] = m.restG[(size_t)m.foot[s]].m[13]; restB[s] = m.restG[(size_t)m.ball[s]].m[13]; }

    for (int k = 0; k < N; ++k) {
        gc.fn(m, (float)k / N, gc.speed);
        float body = gc.speed * dt * k;                     // posicao do corpo no chao
        Frame& f = fr[(size_t)k];
        f.hip = m.G[(size_t)m.thigh[0]].m[13];
        for (int s = 0; s < 2; ++s) {
            const Mat4& th = m.G[(size_t)m.thigh[s]];
            const Mat4& ca = m.G[(size_t)m.calf[s]];
            const Mat4& ft = m.G[(size_t)m.foot[s]];
            const Mat4& bl = m.G[(size_t)m.ball[s]];
            f.ay[s] = ft.m[13] - restA[s];
            f.by[s] = bl.m[13] - restB[s];
            f.gzA[s] = body + ft.m[14];
            f.gzB[s] = body + bl.m[14];
            f.th[s] = angleOf(th, ca);
            f.ca[s] = angleOf(ca, ft);
            f.ft[s] = angleOf(ft, bl);
            f.ar[s] = angleOf(m.G[(size_t)m.upper[s]], m.G[(size_t)m.lower[s]]);
            f.fa[s] = angleOf(m.G[(size_t)m.lower[s]], m.G[(size_t)m.hand[s]]);
        }
    }

    // contato com o chao e deslize
    double slideSum = 0, slideMax = 0; int slideN = 0; int contact[2] = { 0, 0 };
    float minA = 1e9f, minB = 1e9f, maxClear = -1e9f;
    for (int s = 0; s < 2; ++s) {
        for (int k = 0; k < N; ++k) {
            const Frame& a = fr[(size_t)k];
            const Frame& b = fr[(size_t)((k + 1) % N)];
            bool ca = a.ay[s] < 0.012f || a.by[s] < 0.012f;
            bool cb = b.ay[s] < 0.012f || b.by[s] < 0.012f;
            if (ca) contact[s]++;
            minA = std::min(minA, a.ay[s]); minB = std::min(minB, a.by[s]);
            maxClear = std::max(maxClear, a.ay[s]);
            if (ca && cb) {
                bool toeA = a.by[s] < a.ay[s], toeB = b.by[s] < b.ay[s];
                if (toeA != toeB) continue;
                float za = toeA ? a.gzB[s] : a.gzA[s], zb = toeA ? b.gzB[s] : b.gzA[s];
                if (k + 1 == N) zb += gc.speed * dt * N;      // fecha o ciclo: o corpo continua andando
                double v = std::fabs((double)(zb - za) / dt);
                slideSum += v; slideMax = std::max(slideMax, v); ++slideN;
            }
        }
    }
    float hipMin = 1e9f, hipMax = -1e9f, kneeMax = 0;
    for (int k = 0; k < N; ++k) {
        hipMin = std::min(hipMin, fr[(size_t)k].hip); hipMax = std::max(hipMax, fr[(size_t)k].hip);
        for (int s = 0; s < 2; ++s) kneeMax = std::max(kneeMax, fr[(size_t)k].ca[s] - fr[(size_t)k].th[s]);
    }
    // aceleracao angular maxima (rad/s^2): mede quao "brusca" e a animacao
    float acc[4] = { 0, 0, 0, 0 };
    int accAt[4] = { 0, 0, 0, 0 };
    for (int k = 0; k < N; ++k) {
        const Frame& p = fr[(size_t)((k + N - 1) % N)], &c = fr[(size_t)k], &n = fr[(size_t)((k + 1) % N)];
        for (int s = 0; s < 2; ++s) {
            float v[4] = { (n.th[s] - 2 * c.th[s] + p.th[s]), (n.ca[s] - 2 * c.ca[s] + p.ca[s]),
                           (n.ft[s] - 2 * c.ft[s] + p.ft[s]), (n.ar[s] - 2 * c.ar[s] + p.ar[s]) };
            for (int q = 0; q < 4; ++q) {
                float a = std::fabs(v[q]) / (dt * dt);
                if (a > acc[q]) { acc[q] = a; accAt[q] = k * 100 / N; }
            }
        }
    }

    fprintf(out, "[%s]  velocidade %.1f m/s, cadencia %.2f ciclos/s\n", gc.label, gc.speed, gc.cadence);
    fprintf(out, "  contato com o chao: perna E %d%%, perna D %d%%\n", contact[0] * 100 / N, contact[1] * 100 / N);
    fprintf(out, "  deslize do pe apoiado: medio %.2f m/s, maximo %.2f m/s   (0 = pe preso ao chao)\n",
            slideN ? slideSum / slideN : 0.0, slideMax);
    fprintf(out, "  altura do tornozelo vs chao: min %+.3f m, ponta do pe min %+.3f m (negativo = afunda), folga max %.3f m\n",
            minA, minB, maxClear);
    fprintf(out, "  quadril: oscilacao vertical %.3f m;  flexao maxima do joelho %.0f graus\n",
            hipMax - hipMin, kneeMax * 57.2958f);
    float swMin = 1e9f, swMax = -1e9f, flMin = 1e9f, flMax = -1e9f;
    for (int k = 0; k < N; ++k)
        for (int s = 0; s < 2; ++s) {
            const Frame& q = fr[(size_t)k];
            swMin = std::min(swMin, q.ar[s]); swMax = std::max(swMax, q.ar[s]);
            float fl = q.ar[s] - q.fa[s];                       // flexao do cotovelo
            flMin = std::min(flMin, fl); flMax = std::max(flMax, fl);
        }
    fprintf(out, "  braco: balanco do ombro %.0f graus (de %+.0f a %+.0f);  cotovelo flexionado de %.0f a %.0f graus\n",
            (swMax - swMin) * 57.2958f, swMin * 57.2958f, swMax * 57.2958f, flMin * 57.2958f, flMax * 57.2958f);
    fprintf(out, "  aceleracao angular max (rad/s2): coxa %.0f, canela %.0f, pe %.0f, braco %.0f   (no ponto %d%%, %d%%, %d%%, %d%% do ciclo)\n\n",
            acc[0], acc[1], acc[2], acc[3], accAt[0], accAt[1], accAt[2], accAt[3]);
}

static void writeBmp(const char* path, int w, int h, const std::vector<unsigned char>& rgb) {
    FILE* f = fopen(path, "wb");
    if (!f) return;
    unsigned size = 54 + (unsigned)rgb.size();
    unsigned char hdr[54] = { 'B', 'M' };
    memcpy(hdr + 2, &size, 4);
    unsigned off = 54, dib = 40; short planes = 1, bits = 24;
    memcpy(hdr + 10, &off, 4); memcpy(hdr + 14, &dib, 4);
    memcpy(hdr + 18, &w, 4); memcpy(hdr + 22, &h, 4);
    memcpy(hdr + 26, &planes, 2); memcpy(hdr + 28, &bits, 2);
    fwrite(hdr, 1, 54, f);
    // glReadPixels entrega RGB; BMP quer BGR
    std::vector<unsigned char> bgr(rgb);
    for (size_t i = 0; i + 2 < bgr.size(); i += 3) std::swap(bgr[i], bgr[i + 2]);
    fwrite(bgr.data(), 1, bgr.size(), f);
    fclose(f);
}

// 2 linhas (uma por caso) x 8 quadros do ciclo, vistos de lado. Pe da frente do personagem = esquerda da imagem.
static void renderGaitSheet(Model& m, const char* path, const GaitCase* cases, int nCases) {
    const int cols = 8, tileW = 160, tileH = 360;
    int saveW = g_width, saveH = g_height;
    glDisable(GL_FOG);
    glClearColor(0.75f, 0.88f, 1.0f, 1.0f);
    glViewport(0, 0, cols * tileW, nCases * tileH);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    for (int r = 0; r < nCases; ++r) {
        for (int c = 0; c < cols; ++c) {
            const GaitCase& gc = cases[r];
            float cycles = (float)c / cols;
            float body = gc.speed * (cycles / gc.cadence);
            gc.fn(m, cycles, gc.speed);
            g_width = tileW; g_height = tileH;
            glViewport(c * tileW, (nCases - 1 - r) * tileH, tileW, tileH);
            setupView(2.4f * SCALE, 0.95f * SCALE, body * SCALE, 0.0f, 0.95f * SCALE, body * SCALE);
            const float lightPos[4] = { 0.9f, 1.0f, 0.4f, 0.0f };
            glLightfv(GL_LIGHT0, GL_POSITION, lightPos);
            drawGround(0.0f, body * SCALE);
            glPushMatrix();
            glTranslatef(0.0f, 0.0f, body * SCALE);
            glRotatef(0.0f, 0, 1, 0);
            drawModel(m, 1.0f, g_look[0]);
            glPopMatrix();
        }
    }
    g_width = saveW; g_height = saveH;
    std::vector<unsigned char> px((size_t)cols * tileW * nCases * tileH * 3);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadBuffer(GL_BACK);
    glReadPixels(0, 0, cols * tileW, nCases * tileH, GL_RGB, GL_UNSIGNED_BYTE, px.data());
    writeBmp(path, cols * tileW, nCases * tileH, px);
    glEnable(GL_FOG);
}

static void poseGait(Model& m, float cycles, float s) { poseModel(m, cycles, s, 0.0f); }

static void runAnimTest(const std::string& prefix) {
    FILE* out = fopen((prefix + ".txt").c_str(), "w");
    if (!out) return;
    const float walk = WALK_SPEED / SCALE, run = RUN_SPEED / SCALE;
    const GaitCase cases[3] = {
        { "andar", poseGait, walk, gaitCadence(walk) },
        { "correr", poseGait, run, gaitCadence(run) },
        { "passo lento", poseGait, 1.6f, gaitCadence(1.6f) },
    };
    for (int mi = 0; mi < 2; ++mi) {
        if (!g_models[mi].loaded) continue;
        fprintf(out, "=== %s ===\n", mi == 0 ? "homem" : "mulher");
        for (int i = 0; i < 3; ++i) analyzeGait(out, g_models[mi], cases[i]);
    }
    // maos: posicao da 3a falange do indicador esquerdo em relacao a mao (negativo em X = dedo fechando em direcao ao corpo)
    Model& hm = g_models[g_models[0].loaded ? 0 : 1];
    int idxJoint = -1;
    for (size_t i = 0; i < hm.joints.size(); ++i) if (!strcmp(hm.joints[i].name, "index_03_l")) idxJoint = (int)i;
    if (idxJoint >= 0) {
        const float sp[3] = { 0.0f, walk, run };
        const char* nm[3] = { "parado", "andando", "correndo" };
        for (int i = 0; i < 3; ++i) {
            poseModel(hm, 0.0f, sp[i], 0.0f);
            const Mat4& a = hm.G[(size_t)idxJoint]; const Mat4& h = hm.G[(size_t)hm.hand[0]];
            fprintf(out, "dedo indicador (%s): dx %+.3f  dy %+.3f  dz %+.3f\n", nm[i], a.m[12] - h.m[12], a.m[13] - h.m[13], a.m[14] - h.m[14]);
        }
    }
    fclose(out);
    renderGaitSheet(g_models[g_models[0].loaded ? 0 : 1], (prefix + ".bmp").c_str(), cases, 2);
}

// ---------------------------------------------------------------- janela / main

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_KEYDOWN:
        if (wp < 256) g_keys[wp] = true;
        return 0;
    case WM_KEYUP:
        if (wp < 256) g_keys[wp] = false;
        return 0;
    case WM_SYSKEYDOWN:                         // Alt + tecla: segue para o Windows, senao Alt+F4 nao fecharia o jogo
        if (wp < 256) g_keys[wp] = true;
        break;
    case WM_SYSKEYUP:
        if (wp < 256) g_keys[wp] = false;
        break;
    case WM_LBUTTONDOWN:
        g_keys[VK_LBUTTON] = true;
        SetCapture(hwnd);                       // o soltar do botao chega mesmo fora da janela
        return 0;
    case WM_LBUTTONUP:
        g_keys[VK_LBUTTON] = false;
        ReleaseCapture();
        return 0;
    case WM_CHAR:                               // texto digitado (so conta na etapa do nome)
        if (g_state == STATE_NAME) {
            unsigned char c = (unsigned char)wp;
            if (c == 8) {
                if (!g_name.empty()) g_name.pop_back();
            } else if (((c >= 32 && c < 127) || c >= 160) && g_name.size() < MAX_NAME && !(c == ' ' && g_name.empty())) {
                g_name.push_back((char)c);
            }
        } else if (g_state == STATE_JOIN) {             // IP do anfitriao: so numeros e pontos
            char c = (char)wp;
            if (c == 8) { if (!g_ipText.empty()) g_ipText.pop_back(); }
            else if (((c >= '0' && c <= '9') || c == '.') && g_ipText.size() < 15) g_ipText.push_back(c);
        }
        return 0;
    case WM_SETFOCUS:
        g_focused = true;
        return 0;
    case WM_KILLFOCUS:
        g_focused = false;
        for (int i = 0; i < 256; ++i) g_keys[i] = false;
        if (g_state == STATE_PLAY && g_menu == MENU_NONE) { g_menu = MENU_MAIN; g_menuSel = 0; }   // saiu da janela: pausa
        return 0;
    case WM_SIZE:
        g_width = LOWORD(lp);
        g_height = HIWORD(lp);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

// Simulacao sem tela (Tiritos.exe --sim-test <arquivo>): roda o combate contra o bot por alguns segundos, sem desenhar,
// e anota posicoes, vida, municao e estados. Cenario 0: jogador parado. Cenario 1: jogador atira no bot o tempo todo.
static bool g_simQuick = false;     // --quick: pula as partidas simuladas e gera so as imagens

static void runSimTest(const std::string& path, HDC hdc, const Font& big, const Font& mid, const Font& small, const Font& tag) {
    FILE* f = fopen(path.c_str(), "w");
    if (!f) return;
    // imagens de momentos importantes (renderiza o quadro de jogo e le o buffer de tras)
    auto snap = [&](const char* name) {
        glViewport(0, 0, g_width, g_height);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        renderPlay(hdc, small, tag, true);
        std::vector<unsigned char> px((size_t)g_width * g_height * 3);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadBuffer(GL_BACK);
        glReadPixels(0, 0, g_width, g_height, GL_RGB, GL_UNSIGNED_BYTE, px.data());
        writeBmp((path + "_" + name + ".bmp").c_str(), g_width, g_height, px);
    };
    // Para cada mapa: a vista de cima (sul embaixo, 4 pixels por unidade) com os obstaculos por tipo e os pontos de
    // nascimento; a conferencia (nenhum nascimento preso, tudo alcancavel a pe, pecas atravessadas ou com um vao que nao
    // da passagem) e algumas imagens do jogo de varios pontos.
    {
        const int keepMap = g_map;
        const GameState keepState = g_state;
        for (int m = 0; m < NUM_MAPS; ++m) {
            g_map = m;
            ensureWorld();
            buildNav();
            const int S = 4, W = (int)(2 * ROOM_X) * S, H = (int)(2 * ROOM_Z) * S;
            std::vector<unsigned char> px((size_t)W * H * 3);
            auto put = [&](int x, int y, int r, int g, int b) {
                if (x < 0 || y < 0 || x >= W || y >= H) return;
                unsigned char* q = &px[((size_t)y * W + x) * 3];
                q[0] = (unsigned char)r; q[1] = (unsigned char)g; q[2] = (unsigned char)b;
            };
            auto rect = [&](float x0, float z0, float x1, float z1, int r, int g, int b) {
                for (int y = (int)((ROOM_Z - z1) * S); y < (int)((ROOM_Z - z0) * S); ++y)
                    for (int x = (int)((x0 + ROOM_X) * S); x < (int)((x1 + ROOM_X) * S); ++x) put(x, y, r, g, b);
            };
            rect(-ROOM_X, -ROOM_Z, ROOM_X, ROOM_Z, 70, 72, 78);
            for (int iz = 0; iz < NAV_H; ++iz)
                for (int ix = 0; ix < NAV_W; ++ix)
                    if (!g_navFree[iz][ix]) rect(navX(ix) - 1.25f, navZ(iz) - 1.25f, navX(ix) + 1.25f, navZ(iz) + 1.25f, 58, 60, 66);
            if (m == MAP_GALPAO)
                for (int iz = 0; iz < LIGHT_NZ; ++iz)
                    for (int ix = 0; ix < LIGHT_NX; ++ix)
                        rect(lightX(ix) - LIGHT_HX, lightZ(iz) - LIGHT_HZ, lightX(ix) + LIGHT_HX, lightZ(iz) + LIGHT_HZ, 96, 96, 84);
            static const int kc[7][3] = { { 0, 0, 0 }, { 150, 150, 160 }, { 200, 200, 190 }, { 60, 110, 190 }, { 170, 115, 55 },
                                          { 230, 200, 60 }, { 190, 120, 80 } };
            int cross = 0, slit = 0;
            for (int i = NUM_WALLS; i < g_numObs; ++i) {
                const Obstacle& o = g_obs[i];
                rect(o.x - o.hw, o.z - o.hd, o.x + o.hw, o.z + o.hd, kc[o.kind][0], kc[o.kind][1], kc[o.kind][2]);
                for (int k = i + 1; k < g_numObs; ++k) {
                    const Obstacle& b = g_obs[k];
                    float ox = std::min(o.x + o.hw, b.x + b.hw) - std::max(o.x - o.hw, b.x - b.hw);
                    float oz = std::min(o.z + o.hd, b.z + b.hd) - std::max(o.z - o.hd, b.z - b.hd);
                    const float need = 2.0f * PLAYER_RADIUS + 0.2f;
                    if (ox > 0.01f && oz > 0.01f) {
                        ++cross;
                        fprintf(f, "  pecas atravessadas: (%.1f, %.1f) e (%.1f, %.1f), %.1f x %.1f\n", o.x, o.z, b.x, b.z, ox, oz);
                    } else if ((ox > 0.01f && oz < -0.01f && oz > -need) || (oz > 0.01f && ox < -0.01f && ox > -need)) {
                        ++slit;
                        fprintf(f, "  VAO ESTREITO entre (%.1f, %.1f) e (%.1f, %.1f): %.2f\n", o.x, o.z, b.x, b.z, -std::min(ox, oz));
                    }
                }
            }
            int gx, gz, bad = 0;
            nearestFree(0.0f, 25.0f, gx, gz);
            navField(gx, gz);
            int nFree = 0, nReach = 0;
            for (int iz = 0; iz < NAV_H; ++iz)
                for (int ix = 0; ix < NAV_W; ++ix) { if (g_navFree[iz][ix]) ++nFree; if (g_navDist[iz][ix] >= 0) ++nReach; }
            float minPair = 1e9f;
            for (int i = 0; i < NUM_SPAWNS; ++i) {
                float x = SPAWNS[i][0], z = SPAWNS[i][1];
                bool blocked = blockedAt(x, z, PLAYER_RADIUS + 0.6f) || std::fabs(x) > ROOM_X - 2.0f || std::fabs(z) > ROOM_Z - 2.0f;
                bool reach = g_navDist[navIZ(z)][navIX(x)] >= 0;
                if (blocked || !reach) { ++bad; fprintf(f, "NASCIMENTO RUIM %d (%.0f, %.0f): %s\n", i, x, z, blocked ? "preso" : "sem caminho"); }
                for (int k = 0; k < i; ++k)
                    minPair = std::min(minPair, std::sqrt((x - SPAWNS[k][0]) * (x - SPAWNS[k][0]) + (z - SPAWNS[k][1]) * (z - SPAWNS[k][1])));
                rect(x - 1.0f, z - 1.0f, x + 1.0f, z + 1.0f, z > 0.0f ? 80 : 255, z > 0.0f ? 150 : 90, z > 0.0f ? 255 : 80);
            }
            bool menuBlocked = blockedAt(0.0f, 25.0f, 4.5f);    // as telas de escolha precisam deste lugar livre
            rect(-0.8f, 24.2f, 0.8f, 25.8f, 255, 255, 255);          // onde o jogador comeca contra o bot
            writeBmp((path + "_mapa" + std::to_string(m) + ".bmp").c_str(), W, H, px);
            fprintf(f, "mapa %d: %d obstaculos, celulas livres %d, alcancaveis %d, nascimentos %d (ruins %d, menor distancia entre dois %.1f), "
                       "atravessadas %d, vaos estreitos %d, lugar das telas %s, luz calculada em %.0f ms\n",
                    m, g_numObs - NUM_WALLS, nFree, nReach, NUM_SPAWNS, bad, minPair, cross, slit, menuBlocked ? "OCUPADO" : "livre", g_bakeMs);

            g_state = STATE_PLAY;
            g_choice = g_models[0].loaded ? 0 : 1;
            startMatch();
            static const float view[5][4] = {                   // x, z, direcao da camera, inclinacao
                { 0.0f, 25.0f, 0.0f, 0.10f }, { -64.0f, 47.0f, -0.95f, 0.16f }, { 30.0f, -8.0f, 1.57f, 0.10f },
                { 62.0f, -46.0f, 2.3f, 0.30f }, { -20.0f, 20.0f, 3.9f, 0.05f } };
            for (int v = 0; v < 5; ++v) {
                g_player.x = view[v][0]; g_player.z = view[v][1];
                g_player.face = view[v][2] + PI;
                g_camYaw = view[v][2]; g_camPitch = view[v][3];
                g_bot.b.x = g_player.x - std::sin(g_camYaw) * 13.0f + std::cos(g_camYaw) * 2.5f;   // o bot parado na frente
                g_bot.b.z = g_player.z - std::cos(g_camYaw) * 13.0f - std::sin(g_camYaw) * 2.5f;
                g_bot.b.face = g_camYaw;
                poseModel(g_models[g_choice], g_player.walkPhase, g_player.speed, g_time, smooth01(g_armBlend));
                if (g_bot.active) poseModel(g_models[g_bot.model], g_bot.b.walkPhase, g_bot.b.speed, g_time, 1.0f);
                snap(("mapa" + std::to_string(m) + "_" + std::to_string(v)).c_str());
            }
            g_bot = Bot();
        }
        g_map = keepMap;
        ensureWorld();
        g_state = keepState;
        fprintf(f, "\n");
    }
    static const char* stateName[4] = { "PERSEGUE", "ATACA", "REPOSICIONA", "COBERTURA" };
    static const char* scenarioName[5] = { "jogador parado", "jogador atira no bot", "mulher joga, bot com 3 balas",
                                           "jogador correndo de um lado para o outro", "jogador escondido, sem atirar" };
    for (int scenario = 0; scenario < (g_simQuick ? 0 : 5); ++scenario) {
        g_choice = scenario == 2 ? 1 : (g_models[0].loaded ? 0 : 1);   // o cenario 2 testa o outro genero
        g_state = STATE_PLAY;
        g_time = 0.0f;
        startMatch();
        if (scenario == 2) g_bot.ammo = 3;                // forca a recarga para ver o bot se esconder
        if (scenario == 4) { g_player.x = -3.0f; g_player.z = 12.0f; }   // atras do muro central
        fprintf(f, "== cenario %d (%s) ==  bot ativo: %d, modelo do bot: %d\n", scenario, scenarioName[scenario],
                g_bot.active ? 1 : 0, g_bot.model);
        if (scenario == 0 && g_bot.active) {              // a grade de navegacao chega em todas as celulas livres?
            int gx, gz, nFree = 0, nReach = 0;
            nearestFree(g_bot.b.x, g_bot.b.z, gx, gz);
            navField(gx, gz);
            for (int iz = 0; iz < NAV_H; ++iz)
                for (int ix = 0; ix < NAV_W; ++ix) { if (g_navFree[iz][ix]) ++nFree; if (g_navDist[iz][ix] >= 0) ++nReach; }
            fprintf(f, "navegacao: %d celulas livres, %d alcancaveis\n", nFree, nReach);
        }
        const float dt = 1.0f / 60.0f;
        float stuckX = g_bot.b.x, stuckZ = g_bot.b.z, stuckT = 0.0f, firstBotShot = -1.0f;
        int stuckCount = 0;
        int prevHp = g_hp, prevBotHp = g_bot.hp, prevBotAmmo = g_bot.ammo, prevAmmo = g_ammo, botShots = 0, hitsOnPlayer = 0, hitsOnBot = 0;
        bool prevBotDead = false, prevDead = false;
        BotState prevState = g_bot.state;
        float tAttack = -1.0f, tPlayerDead = -1.0f, tBotDead = -1.0f;
        bool snapHurt = false, snapFar = false;
        for (int frame = 0; frame < 60 * 45; ++frame) {
            g_time += dt;
            poseModel(g_models[g_choice], g_player.walkPhase, g_player.speed, g_time, smooth01(g_armBlend));
            if (g_bot.active) poseModel(g_models[g_bot.model], g_bot.b.walkPhase, g_bot.b.speed, g_time, 1.0f);
            g_keys[VK_LBUTTON] = false;
            if (scenario == 1 && g_bot.active && !g_bot.dead && !g_playerDead) {
                g_camPitch = 0.0f;
                for (int it = 0; it < 4; ++it) {        // aponta a mira para o bot, descontando o deslocamento do ombro
                    float t[3];
                    camTarget(g_player, t);
                    g_camYaw = std::atan2(-(g_bot.b.x - t[0]), -(g_bot.b.z - t[2]));
                }
                g_keys[VK_LBUTTON] = true;
            }
            if (scenario == 3) {                        // corre de um lado para o outro
                bool right = ((frame / 150) & 1) == 0;
                g_keys['D'] = right; g_keys['A'] = !right; g_keys[VK_SHIFT] = true;
            }
            update(dt, 0.0f, 0.0f);
            memcpy(g_prevKeys, g_keys, sizeof(g_keys));

            // o bot travou? (quer andar, mas fica tres segundos no mesmo lugar)
            if (g_bot.state == BS_CHASE && !g_bot.dead && !g_playerDead) {
                stuckT += dt;
                if (stuckT > 3.0f) {
                    float mv = std::fabs(g_bot.b.x - stuckX) + std::fabs(g_bot.b.z - stuckZ);
                    if (mv < 0.5f) { ++stuckCount; fprintf(f, "  t=%5.2f  BOT TRAVADO em (%.1f, %.1f)\n", g_time, g_bot.b.x, g_bot.b.z); }
                    stuckX = g_bot.b.x; stuckZ = g_bot.b.z; stuckT = 0.0f;
                }
            } else { stuckX = g_bot.b.x; stuckZ = g_bot.b.z; stuckT = 0.0f; }
            if (g_bot.ammo < prevBotAmmo && firstBotShot < 0.0f) firstBotShot = g_time;

            char nm[32];
            if (!snapFar && g_time > 2.5f) { snapFar = true; snprintf(nm, sizeof(nm), "s%d_chase", scenario); snap(nm); }
            if (g_bot.state == BS_ATTACK && tAttack < 0.0f) tAttack = g_time;
            if (tAttack > 0.0f && g_time > tAttack + 0.5f && tAttack < 1e8f) { snprintf(nm, sizeof(nm), "s%d_attack", scenario); snap(nm); tAttack = 1e9f; }
            if (g_hp < prevHp && !snapHurt) { snapHurt = true; snprintf(nm, sizeof(nm), "s%d_hurt", scenario); snap(nm); }
            if (g_playerDead && tPlayerDead < 0.0f) tPlayerDead = g_time;
            if (tPlayerDead > 0.0f && g_time > tPlayerDead + 1.0f && tPlayerDead < 1e8f) { snprintf(nm, sizeof(nm), "s%d_playerdead", scenario); snap(nm); tPlayerDead = 1e9f; }
            if (g_bot.dead && tBotDead < 0.0f) tBotDead = g_time;
            if (tBotDead > 0.0f && g_time > tBotDead + 0.7f && tBotDead < 1e8f) { snprintf(nm, sizeof(nm), "s%d_botdead", scenario); snap(nm); tBotDead = 1e9f; }
            if (g_hp < prevHp) { ++hitsOnPlayer; fprintf(f, "  t=%5.2f  jogador levou tiro: vida %d\n", g_time, g_hp); }
            if (g_bot.hp < prevBotHp) { ++hitsOnBot; fprintf(f, "  t=%5.2f  bot levou tiro: vida %d\n", g_time, g_bot.hp); }
            if (g_bot.ammo < prevBotAmmo) ++botShots;
            if (g_bot.dead && !prevBotDead) fprintf(f, "  t=%5.2f  BOT ELIMINADO (placar %d x %d)\n", g_time, g_scorePlayer, g_scoreBot);
            if (!g_bot.dead && prevBotDead) fprintf(f, "  t=%5.2f  bot renasceu em (%.1f, %.1f)\n", g_time, g_bot.b.x, g_bot.b.z);
            if (g_playerDead && !prevDead) fprintf(f, "  t=%5.2f  JOGADOR ELIMINADO (placar %d x %d)\n", g_time, g_scorePlayer, g_scoreBot);
            if (!g_playerDead && prevDead) fprintf(f, "  t=%5.2f  jogador renasceu\n", g_time);
            if (g_bot.state != prevState) fprintf(f, "  t=%5.2f  bot: %s -> %s\n", g_time, stateName[prevState], stateName[g_bot.state]);
            if (g_ammo > prevAmmo && prevAmmo == 0) fprintf(f, "  t=%5.2f  jogador recarregou\n", g_time);
            prevHp = g_hp; prevBotHp = g_bot.hp; prevBotAmmo = g_bot.ammo; prevAmmo = g_ammo;
            prevBotDead = g_bot.dead; prevDead = g_playerDead; prevState = g_bot.state;
            if (frame % 120 == 0)
                fprintf(f, "t=%5.1f jogador (%.1f,%.1f) vida %d mun %d | bot (%.1f,%.1f) vida %d mun %d rec %.1f %s%s | tiros no ar %d\n",
                        g_time, g_player.x, g_player.z, g_hp, g_ammo, g_bot.b.x, g_bot.b.z, g_bot.hp, g_bot.ammo, g_bot.reloadTimer,
                        stateName[g_bot.state], g_bot.dead ? " (morto)" : "", (int)g_proj.size());
        }
        fprintf(f, "resumo: tiros do bot %d (primeiro em t=%.1f), acertos no jogador %d, acertos no bot %d, placar %d x %d, travadas %d\n\n",
                botShots, firstBotShot, hitsOnPlayer, hitsOnBot, g_scorePlayer, g_scoreBot, stuckCount);
        g_keys['A'] = g_keys['D'] = g_keys[VK_SHIFT] = g_keys[VK_LBUTTON] = false;
    }

    // clique usado no menu nao pode virar tiro: travado nao atira; solta e aperta de novo, atira
    {
        g_choice = 0;
        startMatch();
        g_bot.active = false;
        const float dt = 1.0f / 60.0f;
        int a0 = g_ammo;
        g_fireLock = true;
        g_keys[VK_LBUTTON] = true;
        for (int i = 0; i < 30; ++i) update(dt, 0.0f, 0.0f);
        int shotsLocked = a0 - g_ammo;
        g_keys[VK_LBUTTON] = false;
        update(dt, 0.0f, 0.0f);
        g_keys[VK_LBUTTON] = true;
        for (int i = 0; i < 30; ++i) update(dt, 0.0f, 0.0f);
        g_keys[VK_LBUTTON] = false;
        fprintf(f, "trava de tiro: %d tiros com o botao preso do menu, %d depois de soltar e apertar\n\n", shotsLocked, a0 - g_ammo);
    }

    // desempenho: tempo de um quadro completo, com os dois personagens e as marcas de tiro na tela
    {
        LARGE_INTEGER fq, a, b;
        QueryPerformanceFrequency(&fq);
        g_choice = 0;
        startMatch();
        g_bot.b.x = 3.0f; g_bot.b.z = 17.0f;
        glViewport(0, 0, g_width, g_height);
        QueryPerformanceCounter(&a);
        for (int i = 0; i < 60; ++i) {
            g_time += 1.0f / 60.0f;
            update(1.0f / 60.0f, 0.0f, 0.0f);
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
            renderPlay(hdc, small, tag, true);
            glFinish();
        }
        QueryPerformanceCounter(&b);
        fprintf(f, "desempenho: %.1f ms por quadro (%d marcas de tiro, %d vertices no homem)\n\n",
                (double)(b.QuadPart - a.QuadPart) * 1000.0 / (double)fq.QuadPart / 60.0, g_decalCount, (int)g_models[0].verts.size());

        // quanto custa cada parte
        double part[3];
        for (int k = 0; k < 3; ++k) {
            QueryPerformanceCounter(&a);
            for (int i = 0; i < 60; ++i) {
                if (k == 1) { poseModel(g_models[0], 0.3f, 1.8f, 0.0f, 1.0f); continue; }
                glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
                setupOrbitCamera(g_player);
                if (k == 0) drawWorld(); else drawDecals();
                glFinish();
            }
            QueryPerformanceCounter(&b);
            part[k] = (double)(b.QuadPart - a.QuadPart) * 1000.0 / (double)fq.QuadPart / 60.0;
        }
        fprintf(f, "  partes: cenario %.1f ms, pose de um personagem %.1f ms, marcas de tiro %.1f ms (placa: %s)\n\n",
                part[0], part[1], part[2], (const char*)glGetString(GL_RENDERER));
    }

    // closes das poses novas, de lado: mira para cima/baixo, coice com clarao, etapas da recarga, pulo e pouso
    {
        g_choice = 0;
        startMatch();
        g_bot.active = false;
        Player c = Player();
        c.x = 0.0f; c.z = 25.0f; c.face = PI * 0.5f;       // de frente para +x; a camera olha de lado
        struct Case { const char* name; float aim, recoil, reload, air, vy, land, flash, armed; };
        static const Case cases[] = {
            { "pose_parado", 0.0f, 0, -1, 0, 0, 0, 0, 1 },      { "pose_mira_cima", -0.40f, 0, -1, 0, 0, 0, 0, 1 },
            { "pose_mira_baixo", 0.55f, 0, -1, 0, 0, 0, 0, 1 }, { "pose_tiro", 0.0f, 1, -1, 0, 0, 0, 0.06f, 1 },
            { "pose_recarga_20", 0.0f, 0, 0.20f, 0, 0, 0, 0, 1 }, { "pose_recarga_32", 0.0f, 0, 0.32f, 0, 0, 0, 0, 1 },
            { "pose_recarga_50", 0.0f, 0, 0.50f, 0, 0, 0, 0, 1 }, { "pose_recarga_78", 0.0f, 0, 0.78f, 0, 0, 0, 0, 1 },
            { "pose_pulo_sobe", 0.0f, 0, -1, 1, 0.8f, 0, 0, 1 }, { "pose_pulo_cai", 0.0f, 0, -1, 1, -0.9f, 0, 0, 1 },
            { "pose_pouso", 0.0f, 0, -1, 0, 0, 0.5f, 0, 1 },     { "pose_desarmado_pulo", 0.0f, 0, -1, 1, 0.5f, 0, 0, 0 },
        };
        for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
            const Case& k = cases[i];
            PoseFx fx;
            fx.aimPitch = k.aim; fx.recoil = k.recoil; fx.reload = k.reload; fx.air = k.air; fx.vy = k.vy; fx.land = k.land;
            c.y = k.air > 0.0f ? 1.2f : 0.0f;
            glViewport(0, 0, g_width, g_height);
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
            setupView(c.x + 1.2f, 2.4f, c.z + 4.6f, c.x + 0.6f, 1.9f, c.z);
            drawWorld();
            Probe pr;
            lightProbe(c.x, c.y, c.z, pr);
            drawShadow(c.x, 0.0f, c.z, pr.direct);
            applyProbe(pr);
            drawCharacter(c, 0, g_look[0], k.armed, k.flash, 0.0f, 0.0f, fx);
            drawLightGlow();
            std::vector<unsigned char> px((size_t)g_width * g_height * 3);
            glPixelStorei(GL_PACK_ALIGNMENT, 1);
            glReadBuffer(GL_BACK);
            glReadPixels(0, 0, g_width, g_height, GL_RGB, GL_UNSIGNED_BYTE, px.data());
            writeBmp((path + "_" + k.name + ".bmp").c_str(), g_width, g_height, px);
        }
    }

    // empunhadura vista de varios angulos, parado, andando e correndo
    {
        Player c = Player();
        c.x = 0.0f; c.z = 25.0f; c.face = PI;               // de frente para -z (o meio do galpao)
        static const float speeds[4] = { 0.0f, 1.8f, 5.0f, 5.0f };
        static const float camA[4] = { 1.5708f, 2.5f, 0.0f, 3.9f };    // lado direito, frente-direita, costas, frente-esquerda
        for (int sp = 0; sp < 4; ++sp)
            for (int v = 0; v < 4; ++v) {
                c.speed = speeds[sp];
                c.walkPhase = 0.30f;
                PoseFx fx;
                fx.fire = sp == 3 ? 1.0f : 0.0f;
                glViewport(0, 0, g_width, g_height);
                glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
                setupView(c.x + std::sin(camA[v]) * 4.4f, 2.5f, c.z + std::cos(camA[v]) * 4.4f, c.x, 1.9f, c.z);
                drawWorld();
                Probe pr;
                lightProbe(c.x, c.y, c.z, pr);
                drawShadow(c.x, 0.0f, c.z, pr.direct);
                applyProbe(pr);
                drawCharacter(c, 0, g_look[0], 1.0f, 0.0f, 0.0f, 0.0f, fx);
                drawLightGlow();
                std::vector<unsigned char> px((size_t)g_width * g_height * 3);
                glPixelStorei(GL_PACK_ALIGNMENT, 1);
                glReadBuffer(GL_BACK);
                glReadPixels(0, 0, g_width, g_height, GL_RGB, GL_UNSIGNED_BYTE, px.data());
                char nm[48];
                snprintf(nm, sizeof(nm), "_hold_%d_%d.bmp", sp, v);
                writeBmp((path + nm).c_str(), g_width, g_height, px);
            }
    }

    // telas de modo de jogo e de partida em rede (sem abrir a rede: so o desenho)
    {
        auto snapScreen = [&](const char* name) {
            glViewport(0, 0, g_width, g_height);
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
            renderLobby(hdc, big, mid, small, tag);
            std::vector<unsigned char> px((size_t)g_width * g_height * 3);
            glPixelStorei(GL_PACK_ALIGNMENT, 1);
            glReadBuffer(GL_BACK);
            glReadPixels(0, 0, g_width, g_height, GL_RGB, GL_UNSIGNED_BYTE, px.data());
            writeBmp((path + "_" + name + ".bmp").c_str(), g_width, g_height, px);
        };
        g_netStatus.clear();
        g_state = STATE_LOBBY; g_lobbySel = 1;
        snapScreen("tela_modo");
        // sala de espera (sem abrir a rede): o anfitriao com tres convidados, nos dois modos, e a vista de um convidado
        static const char* fake[3] = { "Ana", "Bruno", "Carla" };
        auto fakeRoom = [&]() {
            for (int i = 1; i <= 3; ++i) {
                g_rp[i] = Bot();
                g_rp[i].present = true;
                g_rp[i].remote = true;
                g_rp[i].name = fake[i - 1];
                g_rp[i].team = i & 1;
                g_rp[i].model = i & 1;
                g_rp[i].look.hair = i;
                g_rp[i].look.outfit = true;
            }
        };
        g_net = NET_HOST; g_myId = 0; g_myTeam = 0; g_phase = PH_LOBBY; g_killLimit = 0;
        g_localIPs = "192.168.0.10   10.0.0.5";
        fakeRoom();
        g_state = STATE_ROOM;
        g_mode = MODE_FFA;
        snapScreen("tela_sala");
        g_mode = MODE_TEAMS;
        snapScreen("tela_sala_equipes");
        g_net = NET_JOIN; g_myId = 2; g_rp[2] = Bot();
        g_rp[0] = Bot(); g_rp[0].present = true; g_rp[0].name = "Murilo"; g_rp[0].team = 0;
        snapScreen("tela_sala_convidado");
        for (int i = 0; i < MAX_PLAYERS; ++i) g_rp[i] = Bot();
        g_net = NET_NONE; g_myId = 0;
        g_state = STATE_JOIN;
        FoundHost fh;
        memset(&fh, 0, sizeof(fh));
        fh.addr.sin_family = AF_INET;
        fh.addr.sin_addr.s_addr = inet_addr("192.168.0.10");
        strcpy(fh.name, "Murilo");
        fh.players = 3; fh.mode = MODE_TEAMS; fh.phase = PH_PLAY;
        g_found.push_back(fh);
        g_ipText = "";
        snapScreen("tela_entrar");
        g_found.clear();
        g_state = STATE_PLAY;

        // partida em rede (sem rede de verdade): tres jogadores na frente, placar, avisos e o fim da partida
        g_net = NET_HOST; g_myId = 0; g_myTeam = 0; g_mode = MODE_TEAMS; g_matchId = 1; g_phase = PH_PLAY;
        fakeRoom();
        g_choice = 0;
        startMatch();
        static const float pos[3][2] = { { -2.5f, 16.0f }, { 1.5f, 15.0f }, { 5.0f, 16.5f } };
        for (int i = 1; i <= 3; ++i) {
            NetBody nb;
            memset(&nb, 0, sizeof(nb));
            nb.x = pos[i - 1][0]; nb.z = pos[i - 1][1]; nb.arm = 1.0f; nb.hp = MAX_HP; nb.team = (unsigned char)(i & 1);
            nb.kills = (unsigned short)(4 - i); nb.deaths = (unsigned short)i; nb.match = 1;
            netApplyBody(g_rp[i], nb);
        }
        g_myKills = 2;
        feedAdd("Bruno  eliminou  Ana", 0);
        feedAdd("Murilo  eliminou  Carla", 1);
        feedAdd("Carla entrou na sala", 0);
        for (int k = 0; k < 30; ++k) update(1.0f / 60.0f, 0.0f, 0.0f, false);
        g_player.x = 0.0f; g_player.z = 25.0f; g_player.face = PI;
        g_camYaw = 0.0f; g_camPitch = 0.12f;
        for (int k = 0; k < 30; ++k) update(1.0f / 60.0f, 0.0f, 0.0f, false);
        snap("rede_equipes");
        g_keys[VK_TAB] = true;
        snap("rede_placar");
        g_keys[VK_TAB] = false;
        g_mode = MODE_FFA;
        snap("rede_todos");
        g_phase = PH_END; g_winner = 1;
        snap("rede_fim");
        for (int i = 0; i < MAX_PLAYERS; ++i) g_rp[i] = Bot();
        g_feed.clear();
        g_net = NET_NONE; g_phase = PH_LOBBY; g_mode = MODE_FFA; g_winner = -1; g_matchId = 0;
    }

    // camera em lugares apertados: as imagens mostram se ela entra na parede e onde a mira fica
    {
        g_choice = 0;
        startMatch();
        g_bot.active = false;
        static const struct { const char* name; float x, z, yaw, pitch; } views[] = {
            { "cam_aberto",        0.0f, 25.0f, 0.0f, 0.12f },   // sem nada por perto
            { "cam_parede_atras",  0.0f, ROOM_Z - 0.8f, 0.0f, 0.12f },   // de costas para a parede sul
            { "cam_parede_direita", ROOM_X - 0.8f, 0.0f, 0.0f, 0.12f },  // parede leste do lado direito (ombro da camera)
            { "cam_canto",        ROOM_X - 0.8f, ROOM_Z - 0.8f, 0.0f, 0.12f },   // canto da sala
        };
        for (size_t i = 0; i < sizeof(views) / sizeof(views[0]); ++i) {
            g_player.x = views[i].x; g_player.z = views[i].z; g_player.vx = g_player.vz = 0.0f;
            g_camYaw = views[i].yaw; g_camPitch = views[i].pitch;
            float eye[3], fw[3], sr[3], up[3], tgt[3], dirn[3];
            for (int k = 0; k < 90; ++k) update(1.0f / 60.0f, 0.0f, 0.0f);
            cameraRig(g_player, eye, fw, sr, up);
            aimPoint(g_player, tgt, dirn);
            fprintf(f, "%s: olho (%.1f, %.1f, %.1f) dist %.1f ombro %.2f subida %.2f, ponto mirado (%.1f, %.1f, %.1f)%s\n", views[i].name,
                    eye[0], eye[1], eye[2], g_camDist, g_shoulder, g_camLift, tgt[0], tgt[1], tgt[2],
                    std::fabs(eye[0]) > ROOM_X || std::fabs(eye[2]) > ROOM_Z ? "  CAMERA FORA DA SALA" : "");
            snap(views[i].name);
        }
    }
    fclose(f);
}

// Teste de sala na rede com varios jogos abertos neste PC: Tiritos.exe [opcoes] --net-test host|join <arquivo>.
// Opcoes: --players N (o anfitriao comeca quando a sala tiver N jogadores), --teams (modo Equipes), --limit K (abates para
// vencer), --matches M (quantas partidas o anfitriao comeca), --leave T (o convidado sai da sala T s depois de comecar a
// jogar), --name X. Usa so 127.0.0.1, entao nao aciona o firewall. Todos ficam em fila no corredor leste, atiram no
// inimigo visivel mais proximo e anotam o que veem: entradas e saidas, fases da sala, eliminacoes e o placar.
static int   g_testPlayers = 2, g_testMatches = 1;
static float g_testLeave = -1.0f;
static bool  g_testTeams = false;
static int   g_testLimit = 0;

static void runNetTest(bool host, const std::string& path, HDC hdc, const Font& small, const Font& tag) {
    FILE* f = fopen(path.c_str(), "w");
    if (!f) return;
    auto snap = [&](const std::string& name) {
        glViewport(0, 0, g_width, g_height);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        renderPlay(hdc, small, tag, true);
        std::vector<unsigned char> px((size_t)g_width * g_height * 3);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadBuffer(GL_BACK);
        glReadPixels(0, 0, g_width, g_height, GL_RGB, GL_UNSIGNED_BYTE, px.data());
        writeBmp((path + "_" + name + ".bmp").c_str(), g_width, g_height, px);
    };
    auto scoreLine = [&]() {
        std::vector<ScoreRow> rows;
        netScoreRows(rows);
        std::string out;
        for (size_t i = 0; i < rows.size(); ++i) {
            char b[96];
            snprintf(b, sizeof(b), "%s%s %d/%d%s", i ? ", " : "", rows[i].name.c_str(), rows[i].kills, rows[i].deaths,
                     g_mode == MODE_TEAMS ? (rows[i].team == 0 ? " az" : " vm") : "");
            out += b;
        }
        return out;
    };
    static const char* phaseName[3] = { "sala", "partida", "fim" };
    g_choice = host || !g_models[1].loaded ? 0 : 1;
    if (g_name.empty()) g_name = host ? "Anfitriao" : "Convidado";
    g_look[g_choice].hair = host ? 1 : 3;
    if (!netOpen(host, true)) { fprintf(f, "erro ao abrir a rede: %s\n", g_netStatus.c_str()); fclose(f); return; }
    if (host) { g_mode = g_testTeams ? MODE_TEAMS : MODE_FFA; g_killLimit = g_testLimit; }
    g_state = host ? STATE_ROOM : STATE_JOIN;
    fprintf(f, "papel: %s (%s)\n", host ? "anfitriao" : "convidado", displayName().c_str());

    LARGE_INTEGER fq, last, now;
    QueryPerformanceFrequency(&fq);
    QueryPerformanceCounter(&last);
    float t = 0.0f, tPlay = -1.0f, logT = 0.0f, lobbyT = 0.0f, endLogT = -1.0f, quitT = -1.0f;
    int prevHp = MAX_HP, hitsTaken = 0, matchesStarted = 0, prevPhase = -1, prevPlayers = 0, shotsSeen = 0;
    bool loggedJoin = false, prevDead = true, snapped = false;
    float lastFeed = -1.0f;
    size_t prevProj = 0;
    GameState prevState = g_state;
    while (t < 170.0f) {
        Sleep(10);
        MSG msg;                                        // mantem a janela respondendo (senao o Windows a da como travada)
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageA(&msg); }
        QueryPerformanceCounter(&now);
        float dt = std::min((float)(now.QuadPart - last.QuadPart) / (float)fq.QuadPart, 0.05f);
        last = now;
        t += dt;
        g_time += dt;
        netPoll(dt);
        if (g_netLost) { fprintf(f, "t=%6.2f conexao encerrada: %s\n", t, g_netStatus.c_str()); break; }
        if (!host && !g_connected && !g_helloPending && !g_found.empty()) {
            fprintf(f, "t=%6.2f achou a sala de \"%s\" (%d jogadores, %s, %s)\n", t, g_found[0].name, g_found[0].players,
                    MODE_NAMES[g_found[0].mode & 1], phaseName[g_found[0].phase % 3]);
            netConnect(g_found[0].addr);
        }
        if (!host && !g_connected && !g_helloPending && t > 15.0f) { fprintf(f, "nao entrou em 15 s (%s)\n", g_netStatus.c_str()); break; }
        if (!host && g_connected && !loggedJoin) {
            loggedJoin = true;
            fprintf(f, "t=%6.2f entrou na sala como jogador %d, equipe %s; a sala esta em: %s\n", t, g_myId,
                    g_myTeam == 0 ? "azul" : "vermelha", phaseName[g_phase % 3]);
        }
        // o anfitriao comeca quando a sala enche; depois da ultima partida volta a sala e encerra
        if (host && g_phase == PH_LOBBY) {
            lobbyT += dt;
            if (matchesStarted >= g_testMatches) { if (quitT < 0.0f) quitT = t + 2.0f; }
            else if (netPlayers() >= g_testPlayers && lobbyT > 1.5f) {
                if (hostStartMatch()) {
                    ++matchesStarted;
                    fprintf(f, "t=%6.2f anfitriao comecou a partida %d (%s, %d jogadores, meta %d)\n", t, matchesStarted,
                            MODE_NAMES[g_mode], netPlayers(), killLimit());
                } else if (lobbyT > 20.0f) { fprintf(f, "nao conseguiu comecar: %s\n", g_netStatus.c_str()); break; }
            }
        } else lobbyT = 0.0f;
        if (host && lobbyT == 0.0f && matchesStarted == 0 && t > 40.0f) { fprintf(f, "a sala nao encheu em 40 s\n"); break; }
        if (quitT > 0.0f && t > quitT) { fprintf(f, "t=%6.2f anfitriao fecha a sala\n", t); break; }

        if (g_phase != prevPhase) {
            fprintf(f, "t=%6.2f fase: %s", t, phaseName[g_phase % 3]);
            if (g_phase == PH_END) {
                if (g_mode == MODE_TEAMS) fprintf(f, " (venceu a equipe %s)", g_winner == 0 ? "azul" : "vermelha");
                else fprintf(f, " (venceu %s)", playerName(g_winner).c_str());
                endLogT = t + 1.5f;
            }
            fprintf(f, "\n");
            prevPhase = g_phase;
        }
        if (endLogT > 0.0f && t > endLogT) { endLogT = -1.0f; fprintf(f, "t=%6.2f PLACAR FINAL: %s\n", t, scoreLine().c_str()); }
        if (netPlayers() != prevPlayers) { fprintf(f, "t=%6.2f jogadores na sala: %d\n", t, netPlayers()); prevPlayers = netPlayers(); }
        for (size_t i = 0; i < g_feed.size(); ++i)
            if (g_feed[i].t > lastFeed) { fprintf(f, "t=%6.2f  >> %s\n", t, g_feed[i].text.c_str()); lastFeed = g_feed[i].t; }

        netSyncState();
        if (g_state == STATE_PLAY && prevState != STATE_PLAY) {
            if (tPlay < 0.0f) tPlay = t;
            prevDead = true;                            // acabou de nascer: vai para a fila de teste
            fprintf(f, "t=%6.2f entrou na partida %d (mapa %d)\n", t, g_matchId, g_worldMap);
        }
        if (g_state != STATE_PLAY && prevState == STATE_PLAY) fprintf(f, "t=%6.2f voltou para a sala\n", t);
        prevState = g_state;
        if (!host && g_testLeave > 0.0f && tPlay >= 0.0f && t - tPlay > g_testLeave) { fprintf(f, "t=%6.2f saindo da sala\n", t); break; }
        if (g_state != STATE_PLAY) { memcpy(g_prevKeys, g_keys, sizeof(g_keys)); continue; }

        // todos em fila no corredor leste (sem obstaculo entre eles), cada um no seu lugar
        if (!g_playerDead && prevDead) { g_player.x = 41.5f; g_player.z = -21.0f + 6.0f * (float)g_myId; }
        prevDead = g_playerDead;
        int tgt = -1;
        float bd = 1e9f;
        for (int i = 0; i < MAX_PLAYERS; ++i) {
            const Bot& r = g_rp[i];
            if (i == g_myId || !r.present || !r.active || r.dead || !netFoe(i)) continue;
            if (!lineOfSight(g_player.x, g_player.y + 2.0f, g_player.z, r.b.x, r.b.y + 2.0f, r.b.z)) continue;
            float d = std::fabs(r.b.z - g_player.z) + std::fabs(r.b.x - g_player.x);
            if (d < bd) { bd = d; tgt = i; }
        }
        float tp = t - tPlay;
        g_keys['A'] = g_keys['D'] = g_keys[VK_LBUTTON] = false;
        if (!host) { bool r = ((int)(tp + 0.5f * g_myId) & 1) == 0; g_keys['D'] = r; g_keys['A'] = !r; }   // anda de lado
        if (tgt >= 0 && !g_playerDead) {
            g_camPitch = 0.0f;
            for (int it = 0; it < 4; ++it) {
                float tg[3];
                camTarget(g_player, tg);
                g_camYaw = std::atan2(-(g_rp[tgt].b.x - tg[0]), -(g_rp[tgt].b.z - tg[2]));
            }
            g_keys[VK_LBUTTON] = ((int)(tp * 1.3f) + g_myId) % 3 != 0;     // rajadas, cada um num ritmo
        }
        poseModel(g_models[g_choice], g_player.walkPhase, g_player.speed, g_time, smooth01(g_armBlend));
        update(dt, 0.0f, 0.0f, g_phase != PH_END);
        memcpy(g_prevKeys, g_keys, sizeof(g_keys));

        for (size_t i = prevProj; i < g_proj.size(); ++i) if (g_proj[i].owner >= 2) ++shotsSeen;
        prevProj = g_proj.size();
        if (g_hp < prevHp) ++hitsTaken;
        prevHp = g_hp;
        if (!snapped && tp > 8.0f) { snapped = true; snap(host ? "host" : "join_" + std::to_string(g_myId)); }
        logT -= dt;
        if (logT <= 0.0f) {
            logT = 2.0f;
            fprintf(f, "t=%6.1f vida %d | placar: %s | pendentes %d\n", t, g_hp, scoreLine().c_str(),
                    host ? 0 : (int)g_hostLink.outbox.size());
        }
    }
    fprintf(f, "resumo: tiros dos outros vistos %d, levei %d tiros, acertos de companheiro ignorados %d, placar: %s\n",
            shotsSeen, hitsTaken, g_teamHits, scoreLine().c_str());
    g_keys['A'] = g_keys['D'] = g_keys[VK_LBUTTON] = false;
    netClose();
    fclose(f);
}

// Procura um formato de tela com antialiasing (4 ou 2 amostras por pixel). So da para perguntar isso com um contexto
// OpenGL ja criado, entao usa uma janela temporaria e invisivel. Devolve 0 se a placa nao oferecer.
static int chooseMsaaFormat(HINSTANCE hInst) {
    WNDCLASSA wc = {};
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = hInst;
    wc.lpszClassName = "Tiritos_tmp";
    RegisterClassA(&wc);
    HWND w = CreateWindowExA(0, wc.lpszClassName, "", WS_POPUP, 0, 0, 16, 16, NULL, NULL, hInst, NULL);
    if (!w) return 0;
    HDC dc = GetDC(w);
    PIXELFORMATDESCRIPTOR pfd = {};
    pfd.nSize = sizeof(pfd);
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    pfd.cDepthBits = 24;
    int result = 0;
    int basic = ChoosePixelFormat(dc, &pfd);
    HGLRC rc = (basic && SetPixelFormat(dc, basic, &pfd)) ? wglCreateContext(dc) : NULL;
    if (rc && wglMakeCurrent(dc, rc)) {
        typedef BOOL (WINAPI *ChooseFn)(HDC, const int*, const FLOAT*, UINT, int*, UINT*);
        ChooseFn choose = (ChooseFn)(void*)wglGetProcAddress("wglChoosePixelFormatARB");
        for (int samples = 4; choose && samples >= 2 && !result; samples /= 2) {
            const int attr[] = { 0x2001, 1,          // DRAW_TO_WINDOW
                                 0x2010, 1,          // SUPPORT_OPENGL
                                 0x2011, 1,          // DOUBLE_BUFFER
                                 0x2013, 0x202B,     // PIXEL_TYPE = RGBA
                                 0x2014, 32,         // COLOR_BITS
                                 0x2022, 24,         // DEPTH_BITS
                                 0x2041, 1,          // SAMPLE_BUFFERS
                                 0x2042, samples,    // SAMPLES
                                 0 };
            int pf = 0;
            UINT n = 0;
            if (choose(dc, attr, NULL, 1, &pf, &n) && n > 0) result = pf;
        }
        wglMakeCurrent(NULL, NULL);
    }
    if (rc) wglDeleteContext(rc);
    ReleaseDC(w, dc);
    DestroyWindow(w);
    UnregisterClassA(wc.lpszClassName, hInst);
    return result;
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR cmdLine, int) {
    std::string cmd = cmdLine ? cmdLine : "";
    {   // --map N (testes): comeca no mapa N
        size_t mp = cmd.find("--map ");
        if (mp != std::string::npos) {
            g_map = std::max(0, std::min((int)NUM_MAPS - 1, atoi(cmd.c_str() + mp + 6)));
            cmd.erase(mp, 8);
        }
    }
    size_t netPos = cmd.find("--net-test");
    bool netTest = netPos != std::string::npos;
    size_t testPos = cmd.find("--anim-test");
    bool animTest = testPos != std::string::npos;
    size_t simPos = cmd.find("--sim-test");
    bool simTest = simPos != std::string::npos;
    SetProcessDPIAware();   // evita escala/corte da janela em telas com zoom > 100%
    g_player.face = PI;     // comeca de costas para a camera

    char exePath[MAX_PATH];
    GetModuleFileNameA(NULL, exePath, MAX_PATH);
    g_baseDir = exePath;
    g_baseDir = g_baseDir.substr(0, g_baseDir.find_last_of("\\/") + 1);

    WNDCLASSA wc = {};
    wc.style = CS_OWNDC;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hIcon = LoadIcon(hInst, MAKEINTRESOURCE(1));   // icone do game.rc
    wc.lpszClassName = "Tiritos";
    RegisterClassA(&wc);

    // janela de 1280x720, reduzida se a tela for menor, e centralizada
    int scrW = GetSystemMetrics(SM_CXSCREEN), scrH = GetSystemMetrics(SM_CYSCREEN);
    if (!animTest && g_width > scrW - 80)   g_width = scrW - 80;
    if (!animTest && g_height > scrH - 140) g_height = scrH - 140;
    RECT rc = { 0, 0, g_width, g_height };
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
    int winW = rc.right - rc.left, winH = rc.bottom - rc.top;
    HWND hwnd = CreateWindowExA(0, wc.lpszClassName,
        "Tiritos",
        WS_OVERLAPPEDWINDOW | WS_VISIBLE, (scrW - winW) / 2, (scrH - winH) / 2,
        winW, winH, NULL, NULL, hInst, NULL);
    if (!hwnd) return 1;

    HDC hdc = GetDC(hwnd);
    PIXELFORMATDESCRIPTOR pfd = {};
    pfd.nSize = sizeof(pfd);
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    pfd.cDepthBits = 24;
    pfd.iLayerType = PFD_MAIN_PLANE;
    int pf = cmd.find("--noaa") == std::string::npos ? chooseMsaaFormat(hInst) : 0;   // 4x antialiasing, se a placa tiver
    bool msaa = pf != 0;
    if (msaa) {
        PIXELFORMATDESCRIPTOR mp;
        DescribePixelFormat(hdc, pf, sizeof(mp), &mp);
        if (!SetPixelFormat(hdc, pf, &mp)) return 1;
    } else {
        pf = ChoosePixelFormat(hdc, &pfd);
        if (!pf || !SetPixelFormat(hdc, pf, &pfd)) return 1;
    }

    HGLRC rcx = wglCreateContext(hdc);
    if (!rcx || !wglMakeCurrent(hdc, rcx)) return 1;

    // VSync, se o driver oferecer
    typedef BOOL (WINAPI *SwapIntervalFn)(int);
    SwapIntervalFn swapInterval = (SwapIntervalFn)(void*)wglGetProcAddress("wglSwapIntervalEXT");
    if (swapInterval) swapInterval(1);

    if (msaa) glEnable(GLX_MULTISAMPLE);
    const float sky[4] = { 0.08f, 0.09f, 0.11f, 1.0f };      // dentro da sala: fundo e neblina escuros
    glClearColor(sky[0], sky[1], sky[2], 1.0f);
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_NORMALIZE);
    glEnable(GL_COLOR_MATERIAL);
    glColorMaterial(GL_FRONT_AND_BACK, GL_AMBIENT_AND_DIFFUSE);
    glEnable(GL_LIGHTING);
    glEnable(GL_LIGHT0);
    const float lightAmb[4] = { 0.40f, 0.40f, 0.42f, 1.0f };
    const float lightDif[4] = { 0.80f, 0.78f, 0.72f, 1.0f };
    glLightfv(GL_LIGHT0, GL_AMBIENT, lightAmb);
    glLightfv(GL_LIGHT0, GL_DIFFUSE, lightDif);

    glEnable(GL_FOG);
    glFogi(GL_FOG_MODE, GL_LINEAR);
    glFogf(GL_FOG_START, 45.0f);
    glFogf(GL_FOG_END, 210.0f);
    glFogfv(GL_FOG_COLOR, sky);

    const char* files[2] = { "assets\\male.mdl", "assets\\female.mdl" };
    int nLoaded = 0;
    for (int i = 0; i < 2; ++i) {
        if (loadModel(g_models[i], g_baseDir + files[i]) || loadModel(g_models[i], files[i])) ++nLoaded;
    }
    if (nLoaded == 0) g_state = STATE_PLAY;        // sem modelos: joga com o boneco de caixas
    else if (!g_models[0].loaded) g_choice = 1;

    Font fontBig = makeFont(hdc, 44, FW_BOLD);
    Font fontMid = makeFont(hdc, 34, FW_BOLD);
    Font fontSmall = makeFont(hdc, 20, FW_NORMAL);
    Font fontTag = makeFont(hdc, 26, FW_BOLD);

    initTextures();
    ensureWorld();                      // monta o mapa escolhido (o galpao, a nao ser com --map N)
    g_player.x = 0.0f;
    g_player.z = 25.0f;                 // as telas de escolha acontecem aqui, de frente para o meio da arena

    if (netTest) {                      // --net-test host|join <arquivo>
        std::string rest = cmd.substr(netPos + strlen("--net-test"));
        size_t a = rest.find_first_not_of(' ');
        rest = a == std::string::npos ? "" : rest.substr(a);
        bool host = rest.compare(0, 4, "host") == 0;
        auto opt = [&](const char* name) -> std::string {   // valor de uma opcao antes de --net-test
            size_t o = cmd.find(name);
            if (o == std::string::npos || o > netPos) return "";
            std::string v = cmd.substr(o + strlen(name));
            size_t s0 = v.find_first_not_of(' ');
            if (s0 == std::string::npos) return "";
            v = v.substr(s0);
            return v.substr(0, v.find(' '));
        };
        if (!opt("--players").empty()) g_testPlayers = atoi(opt("--players").c_str());
        if (!opt("--matches").empty()) g_testMatches = atoi(opt("--matches").c_str());
        if (!opt("--limit").empty()) g_testLimit = atoi(opt("--limit").c_str());
        if (!opt("--leave").empty()) g_testLeave = (float)atof(opt("--leave").c_str());
        if (!opt("--name").empty()) g_name = opt("--name");
        g_testTeams = cmd.find("--teams") < netPos;
        size_t sp = rest.find(' ');
        std::string path = sp == std::string::npos ? (host ? "net_host.txt" : "net_join.txt") : rest.substr(sp + 1);
        path = path.substr(0, path.find_last_not_of(' ') + 1);
        runNetTest(host, path, hdc, fontSmall, fontTag);
        if (g_wsaReady) WSACleanup();
        wglMakeCurrent(NULL, NULL);
        wglDeleteContext(rcx);
        return 0;
    }
    if (simTest) {
        g_simQuick = cmd.find("--quick") != std::string::npos;
        if (g_simQuick) cmd.erase(cmd.find("--quick"), 7);
        simPos = cmd.find("--sim-test");
        std::string path = cmd.substr(simPos + strlen("--sim-test"));
        size_t a = path.find_first_not_of(' '), b = path.find_last_not_of(' ');
        path = a == std::string::npos ? "sim_test.txt" : path.substr(a, b - a + 1);
        runSimTest(path, hdc, fontBig, fontMid, fontSmall, fontTag);
        wglMakeCurrent(NULL, NULL);
        wglDeleteContext(rcx);
        return 0;
    }
    if (animTest) {
        std::string prefix = cmd.substr(testPos + strlen("--anim-test"));
        size_t a = prefix.find_first_not_of(' '), b = prefix.find_last_not_of(' ');
        prefix = a == std::string::npos ? "anim_test" : prefix.substr(a, b - a + 1);
        runAnimTest(prefix);
        wglMakeCurrent(NULL, NULL);
        wglDeleteContext(rcx);
        return 0;
    }

    LARGE_INTEGER freq, last;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&last);

    g_seed ^= GetTickCount();           // cada partida com um bot um pouco diferente
    bool mouseCaptured = false;         // so durante o jogo: nas telas e no menu o cursor fica livre
    bool firstFrame = true;
    POINT lastMouse = { 0, 0 };

    bool running = true;
    while (running) {
        MSG msg;
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) running = false;
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
        if (!running) break;
        if (IsIconic(hwnd)) {           // minimizada: nao gasta processador nem conta o tempo
            Sleep(50);
            QueryPerformanceCounter(&last);
            continue;
        }
        if (pressed(VK_ESCAPE)) {
            if (g_state == STATE_NAME) g_state = STATE_SELECT;      // volta para o visual
            else if (g_state == STATE_LOBBY) g_state = STATE_NAME;
            else if (g_state == STATE_ROOM || g_state == STATE_JOIN) { netClose(); g_netStatus.clear(); g_state = STATE_LOBBY; }
            else if (g_menu == MENU_CONTROLS) g_menu = MENU_MAIN;
            else if (g_menu == MENU_MAIN) g_menu = MENU_NONE;
            else { g_menu = MENU_MAIN; g_menuSel = 0; }
        }

        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        float dt = (float)(now.QuadPart - last.QuadPart) / (float)freq.QuadPart;
        last = now;
        if (dt > 0.05f) dt = 0.05f;
        g_time += dt;

        // rede: recebe/envia; segue a fase da sala (espera / partida) e volta para o modo de jogo se a conexao cair
        netPoll(dt);
        if (g_netLost) {
            g_netLost = false;
            netClose();
            g_bot = Bot();
            g_state = STATE_LOBBY;
            g_menu = MENU_NONE;
        }
        netSyncState();
        ensureWorld();                  // o mapa escolhido (ou o que o anfitriao mandou) passa a ser o da tela

        // mouse: le o deslocamento em relacao ao centro e recentraliza
        float mdx = 0, mdy = 0;
        bool wantMouse = g_state == STATE_PLAY && g_menu == MENU_NONE && g_focused && GetForegroundWindow() == hwnd;
        if (wantMouse) {
            if (!mouseCaptured) { ShowCursor(FALSE); mouseCaptured = true; firstFrame = true; }
            // o cursor fica preso na janela: um clique de tiro nunca cai em outro programa
            POINT tl = { 0, 0 }, br = { g_width, g_height };
            ClientToScreen(hwnd, &tl);
            ClientToScreen(hwnd, &br);
            RECT clip = { tl.x, tl.y, br.x, br.y };
            ClipCursor(&clip);
            POINT center = { g_width / 2, g_height / 2 };
            ClientToScreen(hwnd, &center);
            POINT cur;
            GetCursorPos(&cur);
            if (!firstFrame) {
                mdx = (float)(cur.x - center.x);
                mdy = (float)(cur.y - center.y);
            }
            SetCursorPos(center.x, center.y);
            firstFrame = false;
        } else if (mouseCaptured) {
            ShowCursor(TRUE);
            ClipCursor(NULL);
            mouseCaptured = false;
        }

        // o menu tambem responde ao mouse
        POINT mp;
        GetCursorPos(&mp);
        ScreenToClient(hwnd, &mp);
        bool mouseMoved = mp.x != lastMouse.x || mp.y != lastMouse.y;
        lastMouse = mp;
        if (g_menu != MENU_NONE) g_fireLock = true;

        if (g_menu == MENU_MAIN) {
            int hover = -1;
            for (int i = 0; i < menuCount(); ++i) {
                float y = menuItemY(i), my = (float)(g_height - mp.y);
                if (my > y - 10.0f && my < y + 36.0f && std::fabs(mp.x - g_width * 0.5f) < 190.0f) hover = i;
            }
            if (hover >= 0 && mouseMoved) g_menuSel = hover;
            if (g_menuSel >= menuCount()) g_menuSel = 0;
            if (pressed(VK_UP) || pressed('W'))   g_menuSel = (g_menuSel + menuCount() - 1) % menuCount();
            if (pressed(VK_DOWN) || pressed('S')) g_menuSel = (g_menuSel + 1) % menuCount();
            bool clicked = pressed(VK_LBUTTON) && hover >= 0;
            if (clicked) g_menuSel = hover;
            if (pressed(VK_RETURN) || clicked) {
                if (g_menuSel == 0) g_menu = MENU_NONE;
                else if (g_menuSel == 1) g_menu = MENU_CONTROLS;
                else if (g_menuSel == 2 && menuCount() == 4) {     // sair da partida sem fechar o jogo
                    g_menu = MENU_NONE;
                    if (g_net == NET_HOST) { g_phase = PH_LOBBY; g_winner = -1; }        // todos voltam para a sala
                    else if (g_net == NET_JOIN) { netClose(); g_state = STATE_LOBBY; g_netStatus = "Voc\xEA saiu da sala."; }
                    else { g_bot = Bot(); g_proj.clear(); g_state = STATE_LOBBY; }
                } else break;
            }
        } else if (g_menu == MENU_CONTROLS) {
            if (pressed(VK_RETURN) || pressed(VK_BACK) || pressed(VK_LBUTTON)) g_menu = MENU_MAIN;
        } else if (g_state == STATE_SELECT) {
            if (pressed(VK_LEFT) || pressed('A'))  { if (g_models[0].loaded) g_choice = 0; }
            if (pressed(VK_RIGHT) || pressed('D')) { if (g_models[1].loaded) g_choice = 1; }
            Look& lk = g_look[g_choice];
            if (pressed(VK_UP) || pressed('W'))   lk.hair = (lk.hair + 1) % NUM_HAIR;
            if (pressed(VK_DOWN) || pressed('S')) lk.hair = (lk.hair + NUM_HAIR - 1) % NUM_HAIR;
            if (pressed('E')) lk.color = (lk.color + 1) % NUM_COLORS;
            if (pressed('Q')) lk.color = (lk.color + NUM_COLORS - 1) % NUM_COLORS;
            if (pressed('B') && g_choice == 0) lk.beard = !lk.beard;
            if (pressed('T')) lk.outfit = !lk.outfit;
            if (pressed(VK_RETURN)) g_state = STATE_NAME;
        } else if (g_state == STATE_NAME) {
            if (pressed(VK_RETURN)) { g_state = STATE_LOBBY; g_netStatus.clear(); }
        } else if (g_state == STATE_LOBBY) {
            if (pressed(VK_UP) || pressed('W'))   g_lobbySel = (g_lobbySel + NUM_LOBBY_ITEMS - 1) % NUM_LOBBY_ITEMS;
            if (pressed(VK_DOWN) || pressed('S')) g_lobbySel = (g_lobbySel + 1) % NUM_LOBBY_ITEMS;
            if (pressed(VK_LEFT) || pressed('A'))  g_map = (g_map + NUM_MAPS - 1) % NUM_MAPS;      // o fundo ja mostra o mapa
            if (pressed(VK_RIGHT) || pressed('D')) g_map = (g_map + 1) % NUM_MAPS;
            if (pressed(VK_RETURN)) {
                if (g_lobbySel == 0) {                  // contra o bot (a escolha de personagem e definitiva)
                    netClose();
                    g_state = STATE_PLAY;
                    startMatch();
                } else if (g_lobbySel == 1) {
                    if (netOpen(true)) { g_localIPs = localIPs(); g_state = STATE_ROOM; }
                } else {
                    if (netOpen(false)) { g_ipText.clear(); g_state = STATE_JOIN; }
                }
            }
        } else if (g_state == STATE_ROOM) {
            // sala de espera: o anfitriao escolhe o modo e comeca; no modo Equipes cada um escolhe a sua com T
            if (g_net == NET_HOST && g_phase == PH_LOBBY) {
                if (pressed(VK_LEFT) || pressed('A') || pressed(VK_RIGHT) || pressed('D')) { g_mode ^= 1; g_netStatus.clear(); }
                if (pressed('M')) g_map = (g_map + 1) % NUM_MAPS;
                if (pressed(VK_RETURN)) hostStartMatch();
            }
            if (pressed('T') && g_mode == MODE_TEAMS && g_phase == PH_LOBBY) g_myTeam ^= 1;
        } else if (g_state == STATE_JOIN) {
            if (pressed(VK_UP) || pressed('W'))   g_foundSel = std::max(g_foundSel - 1, 0);
            if (pressed(VK_DOWN) || pressed('S')) g_foundSel = std::min(g_foundSel + 1, std::max((int)g_found.size() - 1, 0));
            if (pressed(VK_RETURN) && !g_helloPending) {
                if (!g_ipText.empty()) {
                    unsigned long ip = inet_addr(g_ipText.c_str());
                    if (ip == INADDR_NONE || std::count(g_ipText.begin(), g_ipText.end(), '.') != 3) {
                        g_netStatus = "IP invalido. Exemplo: 192.168.0.10";
                    } else {
                        sockaddr_in to;
                        memset(&to, 0, sizeof(to));
                        to.sin_family = AF_INET;
                        to.sin_port = htons(NET_PORT);
                        to.sin_addr.s_addr = ip;
                        netConnect(to);
                    }
                } else if (!g_found.empty()) {
                    netConnect(g_found[(size_t)g_foundSel].addr);
                }
            }
        } else {
            update(dt, mdx, mdy, !(g_net != NET_NONE && g_phase == PH_END));   // fim da partida: so olha o placar
        }
        // na rede o jogo nao para com o menu aberto (o outro continua jogando): so os controles ficam parados
        if (g_state == STATE_PLAY && g_menu != MENU_NONE && g_net != NET_NONE) update(dt, 0.0f, 0.0f, false);

        glViewport(0, 0, g_width, g_height);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        bool ui = (g_menu == MENU_NONE);
        if (g_state == STATE_SELECT)    renderSelect(hdc, fontBig, fontMid, fontSmall, ui);
        else if (g_state == STATE_NAME) renderName(hdc, fontBig, fontSmall, fontTag);
        else if (g_state == STATE_PLAY) renderPlay(hdc, fontSmall, fontTag, ui);
        else                            renderLobby(hdc, fontBig, fontMid, fontSmall, fontTag);
        if (g_menu != MENU_NONE) renderMenu(hdc, fontBig, fontMid, fontSmall);

        SwapBuffers(hdc);
        memcpy(g_prevKeys, g_keys, sizeof(g_keys));
    }

    if (mouseCaptured) { ShowCursor(TRUE); ClipCursor(NULL); }
    netClose();                         // avisa o outro jogador que saiu
    if (g_wsaReady) WSACleanup();
    wglMakeCurrent(NULL, NULL);
    wglDeleteContext(rcx);
    ReleaseDC(hwnd, hdc);
    DestroyWindow(hwnd);
    return 0;
}
