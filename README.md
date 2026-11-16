# Tiritos

Jogo de tiro em terceira pessoa para Windows, feito em C++ com OpenGL. A partida acontece numa arena de 150 × 110 m, em um de três mapas.

- **Contra o bot:** você enfrenta um bot do gênero oposto.
- **Em rede (LAN):** salas com até 8 jogadores. Quem cria a sala escolhe o modo: **Todos contra todos**, **Equipes** (azul × vermelha) ou **Captura da bandeira**. Aperte Tab para ver o placar.

## Captura da bandeira

Cada equipe tem uma base com a sua bandeira, uma em cada ponta do mapa, e renasce só nela. Encoste na bandeira inimiga para pegá-la e leve até a sua base: vale 1 ponto, desde que a sua bandeira esteja em casa. Vence a equipe que fizer 3 capturas.

Quem leva a bandeira e é eliminado a deixa cair. O inimigo pode pegá-la de novo, um companheiro a devolve encostando nela, e sozinha ela volta para a base em 25 segundos.

## Mapas

- **Galpão:** fechado e iluminado por cima, com muros, caixotes e containers.
- **Deserto:** a céu aberto e árido, entre rochas, ruínas e sacos de areia.
- **Sala branca:** tudo branco e com névoa; de longe quase não se vê nada.

O mapa é escolhido na tela "Modo de jogo" (A / D). Na rede vale o mapa de quem criou a sala, que também pode trocá-lo na sala com M.

## Baixar e jogar

Pegue o `Tiritos_Setup.exe` (instalador) ou o `Tiritos_portatil.zip` (sem instalar) na página **Releases** deste repositório.

## Compilar

Você precisa do MinGW (g++), por exemplo o [WinLibs](https://winlibs.com/), ou do Visual Studio (cl).

```bat
build.bat
```

O resultado é o `Tiritos.exe`, que fica nesta pasta e lê os modelos em `assets\`.

Para gerar o instalador e a versão portátil em `dist\`:

```bat
tools\make_release.bat
```

## Testes

```bat
Tiritos.exe --anim-test <prefixo>
Tiritos.exe [--map N] [--quick] --sim-test <arquivo>
Tiritos.exe [--map N] [--players N] [--teams | --ctf] [--limit K] [--matches M] --net-test host|join <arquivo>
```

- `--anim-test` testa o ciclo de passos.
- `--sim-test` simula o combate contra o bot e gera o mapa e as telas.
- `--net-test` testa uma sala em rede usando só 127.0.0.1.
- `--map N` escolhe o mapa do teste (0 galpão, 1 deserto, 2 sala branca).

## Organização

| Caminho | Conteúdo |
|---|---|
| `main.cpp` | o jogo inteiro |
| `game.rc` | ícone e versão do executável |
| `assets/` | modelos convertidos (`*.mdl`) e ícone |
| `assets/raw/` | modelos originais (glTF) |
| `tools/convert_model.py` | converte de `assets/raw/` para `*.mdl` |
| `tools/setup/` | instalador e desinstalador |

## Créditos

Personagens do *Universal Base Characters* de [Quaternius](https://quaternius.com), com licença CC0 (veja `assets/raw/License_Standard.txt`).
