@echo off
rem Gera assets\male.mdl e assets\female.mdl a partir de assets\raw (glTF + PNG).
cd /d "%~dp0\.."
set H=assets\raw\hair

python -I tools\convert_model.py assets\raw\Superhero_Male_FullBody.gltf assets\male.mdl ^
  --acc 2:1:%H%\Hair_Buzzed.gltf --acc 2:2:%H%\Hair_SimpleParted.gltf --acc 2:3:%H%\Hair_Long.gltf ^
  --acc 2:4:%H%\Hair_Buns.gltf --acc 3:0:%H%\Hair_Beard.gltf

python -I tools\convert_model.py assets\raw\Superhero_Female_FullBody.gltf assets\female.mdl ^
  --acc 2:1:%H%\Hair_BuzzedFemale.gltf --acc 2:2:%H%\Hair_SimpleParted.gltf --acc 2:3:%H%\Hair_Long.gltf ^
  --acc 2:4:%H%\Hair_Buns.gltf
