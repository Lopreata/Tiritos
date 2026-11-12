# Tiritos

Jogo de tiro em terceira pessoa para Windows, feito em C++ com OpenGL. A partida acontece num galpão fechado de 150 × 110 m.

- **Contra o bot:** você enfrenta um bot do gênero oposto.
- **Em rede (LAN):** salas com até 8 jogadores. Quem cria a sala escolhe o modo, **Todos contra todos** ou **Equipes** (azul × vermelha). Aperte Tab para ver o placar.

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
Tiritos.exe [--quick] --sim-test <arquivo>
Tiritos.exe [--players N] [--teams] [--limit K] [--matches M] --net-test host|join <arquivo>
```

- `--anim-test` testa o ciclo de passos.
- `--sim-test` simula o combate contra o bot e gera o mapa e as telas.
- `--net-test` testa uma sala em rede usando só 127.0.0.1.

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
