

#ifndef __PINDEFINE_H__
#define __PINDEFINE_H__

//==============  Standard SPI Setting   ==============//
// Please modify the pin number
#define SPI_CS0 18
#define SPI_CS1 17
#define SPI_CLK 9
#define SPI_Data0 41
#define SPI_Data1 40
#define SPI_Data2 39
#define SPI_Data3 38

//==============   GPIO Setting   ==============//
// Please modify the pin number
#define EPD_BUSY 7 // Please set it as input pin
#define EPD_RST 6  // Please set it as output pin
#define LOAD_SW 45 // Please set it as output pin

//==============  Switch Setting  ==============//
#define SW_2 13
#define SW_4 21

//==============  SD Card Setting  ==============//
// SD card has its own SPI data pins (GPIO 3, 4) but shares
// GPIO 18 with the display (display CS0 = SD SCK).
// Access SD card BEFORE display init, then release the bus.
#define SD_CS 15  // SD card chip select
#define SD_MOSI 3 // SD card CMD (data in)
#define SD_MISO 4 // SD card D0  (data out)
#define SD_SCK 18 // SD card clock (SHARED with SPI_CS0!)

//===============================================

#define GPIO_LOW 0
#define GPIO_HIGH 1

#endif // #ifndef __PINDEFINE_H__
