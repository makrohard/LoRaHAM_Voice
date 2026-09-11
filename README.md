![LoRaHAM_Pi](https://github.com/LoRaHAM/LoRaHAM_Pi/blob/main/LoRaHAM_logo.png?raw=true)

# LoRaHAM_Voice (english) - Speech over LoRa (Codec 2)

LoRaHAM_Voice is a software for the LoRaHAM_Pi hardware upgrade project and LoRaHAM modules for amateur radio operators, enabling high-power LoRa operation with long range on a single-board computer. The daemon is a device driver that allows users (without any hardware programming knowledge) to easily operate the system.

First code for the LoRaHAM Pi hardware | https://www.loraham.de/produkt/loraham-pi/

U need an Audio-Adapter with Microphone and Speaker or an BT-Headset! Not all Headsets will support bei RPi OS. ML18 works, EOTE14 not (both tested)

<img src="https://github.com/LoRaHAM/LoRaHAM_Pi/blob/main/LoRaHAM_P1_3.jpg" alt="LoRaHAM_Pi" width="300" height="auto"><img src="https://github.com/LoRaHAM/LoRaHAM_Ressources/blob/main/LoRaHAM_Cartridge_for_pi500.png" alt="LoRaHAM Cartridge" width="300" height="auto">

* Raspberry Pi 3/4/5
* Raspbian Image on RPi
* LoRaHAM_Daemon | https://github.com/LoRaHAM/LoRaHAM_Daemon

# Need follow parts on the Raspberry Pi image:

    1st: The Daemon:
    https://github.com/LoRaHAM/LoRaHAM_Daemon

    2nd:
    sudo apt update
    sudo apt install libcodec2-dev -y
    sudo apt install libasound2-dev -y
    sudo apt install libgtk-3-dev -y
    sudo apt install libncurses5-dev -y
     
    git clone https://github.com/LoRaHAM/LoRaHAM_Voice ~/LoRaHAM/voice


# Compile instruction
loraham_voice:

    cd ~/LoRaHAM/voice
    gcc -o loraham_voice loraham_voice.c `pkg-config --cflags --libs gtk+-3.0` -lcodec2 -lasound -lncurses -lpthread -lm


# Use instructions:
1. first run the LoRaHAM Daemon because this is the interface between hardware (LoRaHAM_Pi HAT or LoRaHAM Cartridge) and users programm
2. then the LoRaHAM Voice 

1. ./loraham_daemon
2. ./loraham_voice

Daemon can also run as real daemon (parameter -d):
1. ./loraham_daemon -d

if you dont run loraham_daemon as a daemon, you see all traffic on your terminal!

Voice options:

     ./loraham_voice        (Auto: GUI wenn DISPLAY gesetzt)
     ./loraham_voice --cli   (CLI erzwingen)
     ./loraham_voice --gui   (GUI erzwingen)
 
Example:

     ./loraham_voice 
 
# Background information
loraham_voice uses 4 IPC (inter process communication) UNIX-Sockets for two LoRa-Bands:

    - DATA868_SOCKET "/tmp/lora868.sock"
    - DATA433_SOCKET "/tmp/lora433.sock"
    - CONF868_SOCKET "/tmp/loraconf868.sock"
    - CONF433_SOCKET "/tmp/loraconf433.sock"
    
After start, you will see an overview of usable audio devices. 
Use "pulse" for pulse-audio

<img src="https://github.com/LoRaHAM/LoRaHAM_Voice/blob/main/Screenshot_LoRaHAM_Voice.png" alt="LoRaHAM_Voice mainscreen" width="300" height="auto"><img src="https://github.com/LoRaHAM/LoRaHAM_Voice/blob/main/Screenshot_LoRaHAM_Voice_devices.png" alt="LoRaHAM_Voice audio" width="300" height="auto">

# Warnings
This code is provided at your own risk and responsibility. This code is experimental.
For radio amateur or laboratory use only.

# Credits and license

    Copyright (c) 2020-2026 Alexander Walter
    Licensed under GPL v3 (text)
    Maintained by Alexander Walter 
    
This project is licensed under the **GNU General Public License v3**. See
[the licence text](https://www.gnu.org/licenses/gpl-3.0.html).

    * **Warranty:** Software is provided "as is", without warranty of any kind.

![LoRaHAM_Pi](https://github.com/LoRaHAM/LoRaHAM_Pi/blob/main/LoRaHAM_logo.png?raw=true)

# LoRaHAM_Voice (deutsch) - Speech over LoRa (Codec 2)

LoRaHAM_Voice ist eine Software für das LoRaHAM_Pi-Hardware-Upgrade-Projekt und LoRaHAM-Module für Funkamateure, die einen leistungsstarken LoRa-Betrieb mit großer Reichweite auf einem Einplatinencomputer ermöglicht. Der Daemon ist ein Gerätetreiber, der es Benutzern (ohne jegliche Kenntnisse in der Hardwareprogrammierung) ermöglicht, das System einfach zu bedienen.

Erster Code für die LoRaHAM Pi Hardware | https://www.loraham.de/produkt/loraham-pi/

Sie benötigen einen Audioadapter mit Mikrofon und Lautsprecher oder ein Bluetooth-Headset! Nicht alle Headsets werden von RPi OS unterstützt. Das ML18 funktioniert, das EOTE14 hingegen nicht (beide getestet).

<img src="https://github.com/LoRaHAM/LoRaHAM_Pi/blob/main/LoRaHAM_P1_3.jpg" alt="LoRaHAM_Pi" width="300" height="auto"><img src="https://github.com/LoRaHAM/LoRaHAM_Ressources/blob/main/LoRaHAM_Cartridge_for_pi500.png" alt="LoRaHAM Cartridge" width="300" height="auto">

* Raspberry Pi 3/4/5
* Raspbian Image auf RPi
* LoRaHAM_Daemon | https://github.com/LoRaHAM/LoRaHAM_Daemon
 
    
# Anderenfalls werden folgende Pakete auf dem Raspberry Pi Image benötigt:

    1.:
    https://github.com/LoRaHAM/LoRaHAM_Daemon
    
    2.:
    sudo apt update
    sudo apt install libcodec2-dev -y
    sudo apt install libasound2-dev -y
    sudo apt install libgtk-3-dev -y
    sudo apt install libncurses5-dev -y
     
    git clone https://github.com/LoRaHAM/LoRaHAM_Voice ~/LoRaHAM/voice
    

# Kompilieranweisung
loraham_voice:

    cd ~/LoRaHAM/voice
    gcc -o loraham_voice loraham_voice.c `pkg-config --cflags --libs gtk+-3.0` -lcodec2 -lasound -lncurses -lpthread -lm

    
# Bedienungsanleitung:
1. Zuerst den LoRaHAM Daemon starten, da dies die Schnittstelle zwischen der Hardware (LoRaHAM_Pi HAT oder LoRaHAM Cartridge) und dem Benutzerprogramm ist.
2. Dann das LoRaHAM Voice 

1. ./loraham_daemon
2. ./loraham_voice

Daemon kann auch als echter Daemon laufen (Parameter -d):

1. ./loraham_daemon -d

Wenn Sie loraham_daemon nicht als Daemon ausführen, sehen Sie den gesamten Datenverkehr in Ihrem Terminal!

Voice Optionen:

     ./loraham_voice        (Auto: GUI wenn DISPLAY gesetzt)
     ./loraham_voice --cli   (CLI erzwingen)
     ./loraham_voice --gui   (GUI erzwingen)

Beispiel:

     ./loraham_voice
 
# Hintergrundinformationen
loraham_voice verwendet 4 IPC (Inter-Process Communication) UNIX-Sockets für zwei LoRa-Bänder:

    - DATA868_SOCKET "/tmp/lora868.sock"
    - DATA433_SOCKET "/tmp/lora433.sock"
    - CONF868_SOCKET "/tmp/loraconf868.sock"
    - CONF433_SOCKET "/tmp/loraconf433.sock"

    
Nach dem Start sehen Sie die Auswahl der Audiogeräte.
Verwenden Sie "pulse" für pulse-audio

<img src="https://github.com/LoRaHAM/LoRaHAM_Voice/blob/main/Screenshot_LoRaHAM_Voice.png" alt="LoRaHAM_Voice mainscreen" width="300" height="auto"><img src="https://github.com/LoRaHAM/LoRaHAM_Voice/blob/main/Screenshot_LoRaHAM_Voice_devices.png" alt="LoRaHAM_Voice audio" width="300" height="auto">
    

# Warnungen
Dieser Code wird auf eigenes Risiko und eigene Verantwortung zur Verfügung gestellt. Dieser Code ist experimentell.
Er ist nur für Funkamateure oder Labore geeignet.

# Credits und Lizenz

    Copyright (c) 2020-2025 Alexander Walter
    Licensed under GPL v3 (text)
    Maintained by Alexander Walter 
    
Dieses Projekt steht unter der **GNU General Public License v3**. Siehe
[den Lizenztext](https://www.gnu.org/licenses/gpl-3.0.html).

    * **Haftung:** Die Software wird "wie besehen" bereitgestellt, ohne jegliche Gewährleistung.
