/*--------------------------------------------------------------------------------------

 pcf8574.h - PCF8574 IO Expander Driver For ESP32
               Written with ESP IDF

 Copyright (C) 2022 Figo Arzaki Maulana

--------------------------------------------------------------------------------------*/

#ifndef PCF8574_H
#define PCF8574_H
#include <Wire.h>
#include <Arduino.h>

class pcf8574
{
public:
    pcf8574(){
    }
    ~pcf8574(){
    }
    void init(uint8_t _addr); // Must be called after i2c_driver_install()

    void writeByte(uint8_t data);
    void write(uint8_t pin,bool data);

    uint8_t readByte();
    bool read(uint8_t pin);

    bool readFast(uint8_t pin); // Read value from ioRegister variable instead of read from pcf8574
    uint8_t readByteFast(); // Read value from ioRegister variable instead of read from pcf8574
private:
    uint8_t ioRegister; // Variable to store the value of the byte of the IO port
    uint8_t address;
};
#endif