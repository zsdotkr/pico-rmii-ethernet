# Under-Construction

Enhanced version of RMII Ethernet MAC functionality for RP2040.
* Complement RX pio to meet the RMII v1.2 CRS/DV characteristics
* Optimize receiver-side code to handle burst packet


## Issues in original repository
RMII RX side SM is incomplete to meet CRS/DV pattern of RMII V1.2
* Most Ethernet PHY adopt RMII V1.2
* LAN8720 also seems to adopt RMII v1.2 even though it does not mention it in datasheet
* CRS/DV output can be toggled at end of frame if RMII v1.2 is adopted
![image](doc/crsdv.jpg)
* CRS/DV toggling occur more frequently with larger frames or higher frame rates.
* So, ***Using CRS/DV as an interrupt source to determine the end-of-frame*** in original repo is inadequate
* Refer [AN-1405 from TI] for more details about characteristics of CRS/DV
* Original Implementation also has packet loss when test with `ping RP2040_IP -s 1450 -i 0.1`

[AN-1405 from TI]: https://www.ti.com/lit/an/snla076a/snla076a.pdf?ts=1672269799540&ref_url=https%253A%252F%252Fwww.google.com%252F



## Compile
1. Fork or clone this repository and move to top directory
1. Run `git submodule update --init --recursive` to clone `lib/lwip` repository source
1. Create `build` directory and move to it
1. Run `cmake ..`
1. Change `pico_lwip` to `lwip_pico_n` in CMakeLists.txt if you meet `add_library cannot create target 'pico_lwip' ...` error while running `cmake ..`
1. Run `make` (at `build` directory)



# pico-rmii-ethernet

Enable 100Mbit/s Ethernet connectivity on your [Raspberry Pi Pico](https://www.raspberrypi.org/products/raspberry-pi-pico/) with an RMII based Ethernet PHY module.

Leverages the Raspberry Pi RP2040 MCU's PIO, DMA, and dual core capabilities to create a Ethernet MAC stack in software!

## Hardware

* [Raspberry Pico](https://www.raspberrypi.org/products/raspberry-pi-pico/)
* Any RMII based Ethernet PHY module, such as the [Waveshare LAN8720 ETH Board](https://www.waveshare.com/lan8720-eth-board.htm)

## Modification to do

![image](https://user-images.githubusercontent.com/159235/147747551-1fca8e2f-e9c8-4833-9947-0a49e2bda6a9.png)
![schematic](https://user-images.githubusercontent.com/159235/147748051-1d8e7100-147f-4f92-9b2f-91e2398c6e03.jpg)


We're generating the 50MHz RMII clock on the RP2040 instead of getting it from the LAN8720A crystal. For that, we remove the two R12 and R14 resistors, and connect one of them back on the two top pads instead to avoid connecting the onboard crystal to the clock, and instead connect the nINT/RETCLK pin from the connector to the XTAL1/CLKIN pin of the LAN8720A chip.

### Wiring

| RMII Module | Raspberry Pi Pico | Library Default |
| ----------- | ----------------- | --------------- |
| TX1 | TX0 + 1 | 11 |
| TX-EN | TX0 + 2 | 12 |
| TX0 | any GPIO | 10 |
| RX0 | any GPIO | 6 |
| RX1 | RX0 + 1 | 7 |
| nINT / RETCLK | 21/23/24/25 | 23 |
| CRS | RX0 + 2 | 8 |
| MDIO | any GPIO | 14 |
| MDC | MDIO + 1 | 15 |
| VCC | 3V3 | |
| GND | GND | |

## Examples

See [examples](examples/) folder. [LWIP](https://www.nongnu.org/lwip/) is included as the TCP/IP stack.

# Current Limitations

* Built-in LWIP stack is compiled with `NO_SYS` so LWIP Netcon and Socket API's are not enabled
* Auto-negotiation to 10BASE-T is not supported
* MDIO is bit-banged