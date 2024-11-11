### Build Prerequisites

Run the following commands to install build tools required by the PiPedal build.

    # install CMake
    sudo apt update
    sudo apt install -y cmake ninja-build

The PiPedal build process also requires version 12 or later of `node.js`. Type `node --version` to see if you have a version 
of `node.js` installed already. Otherwise run the following commands as root to install the v14.x lts version of `node.js`: 
`
   sudo apt install nodejs npm

If your distribution doesn't provide a suitable version of nodejs, you can install the current LTS version of nodejs
with

    # install NodeJS latest LTS release.
    curl -fsSL https://deb.nodesource.com/setup_20.x | sudo -E bash -
    sudo apt-get install -y nodejs


Run the following commands to install dependent libraries required by the PiPedal build.
 
    sudo apt update 
    sudo apt install -y liblilv-dev libboost-dev jackd2 libjack-jackd2-dev libnl-3-dev libnl-genl-3-dev libsystemd-dev catch 
    sudo apt install -y libasound2-dev jackd2 uuid-dev libxrandr-dev 
    sudo apt install -y authbind 
    sudo apt install -y libavahi-client-dev 
    sudo apt install -y libnm-dev libicu-dev 
    sudo apt install -y libsdbus-c++-dev libzip-dev google-perftools libgoogle-perftools-dev
    

### Installing Sources

If you are using Visual Studio Code, install the following Extensions:

- C/C++ Extension Pack (Microsoft).
- CMake Tools (Microsoft)

Clone the rerdavies/pipdal package from github. Use the source control tools from Visual Studio Code, or 
   
    cd ~/src    # or whereever you keep your source repositories.
    git clone https://github.com/rerdavies/pipedal.git 
   
In the project root, run the following commands to initialze and update pipedal submodules. These steps 
must be performed even if you used Visual Studio Code to initially install the project.

    cd ~/src/pipedal
    git submodule update --init --recursive
   
Run the following command to install and configure React dependencies.

    cd ~/src/pipedal
    ./react-config   # Configure React NPM dependencies.
   
--------------------------   
[<< Building PiPedal from Source](BuildingPiPedalFromSource.md) | [Up](Documentation.md) | [The Build System >>](TheBuildSystem.md)
 
