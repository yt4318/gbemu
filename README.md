# gbemu
For learning purpose referencing LLD_gbemu  
https://github.com/rockytriton/LLD_gbemu/

Adding APU using Kiro IDE × Claude Opus 4.5.

# How to run (on Ubuntu 20.04/22.04)
sudo apt install libsdl2-dev  
sudo apt install libsdl2-ttf-dev  
sudo apt install build-essential  
sudo apt install check  
sudo apt install git  
sudo apt install cmake  
sudo mkdir gbemu  
cd gbemu  
git clone https://github.com/yt4318/gbemu  
cd gbemu/emu  
sudo mkdir build  
cd build  
sudo cmake ..  
sudo make  
gbemu/gbemu ../../roms/<rom_file>  

## Reference 
Pan Docs
https://gbdev.io/pandocs/

Game Boy / Color Architecture
https://www.copetti.org/writings/consoles/game-boy/

Gameboy CPU (LR35902) instruction set
https://www.pastraiser.com/cpu/gameboy/gameboy_opcodes.html

GameBoy CPU Manual
http://marc.rawer.de/Gameboy/Docs/GBCPUman.pdf

Game Boy: Complete Technical Reference
https://gekkio.fi/files/gb-docs/gbctr.pdf

Z80 CPU User Manual
https://www.zilog.com/docs/z80/um0080.pdf

The Ultimate Game Boy Talk (33c3)
https://www.youtube.com/watch?v=HyzD8pNlpwI&t=3651s

GBEDG　　The Gameboy Emulator Development Guide
https://hacktix.github.io/GBEDG/

akatsuki105/gb-docs-ja
https://github.com/akatsuki105/gb-docs-ja

Rustで作るGAME BOYエミュレータ
https://techbookfest.org/product/sBn8hcABDYBMeZxGvpWapf?productVariantID=2q95kwuw4iuRAkJea4BnKT
