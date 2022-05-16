#include <pcf8574.h>

/* Initialize PCF8574 with specific address and i2c port
Must be called after Wire.begin()
*/
void pcf8574::init(uint8_t _addr)
{
    address = _addr;
    writeByte(0x00);
}

void pcf8574::writeByte(uint8_t data)
{
    Wire.beginTransmission(address);
    Wire.write(data);
    Wire.endTransmission(true);
    ioRegister = data;
}

void pcf8574::write(uint8_t pin, bool data)
{
    ioRegister = data ? ioRegister | (1 << pin) : ioRegister & ~(1 << pin);
    return writeByte(ioRegister);
}

uint8_t pcf8574::readByte()
{
    uint8_t data;
    Wire.requestFrom(address,sizeof(uint8_t));
    data = Wire.read();
    Wire.endTransmission(true);
    ioRegister = data;
    return data;
}

bool pcf8574::read(uint8_t pin)
{
    return bitRead(readByte(),pin);
}

// Read value from ioRegister variable instead of read from pcf8574
bool pcf8574::readFast(uint8_t pin)
{
    return (((ioRegister) >> (pin)) & 0x01);
}

// Read value from ioRegister variable instead of read from pcf8574
uint8_t pcf8574::readByteFast()
{
    return ioRegister;
}