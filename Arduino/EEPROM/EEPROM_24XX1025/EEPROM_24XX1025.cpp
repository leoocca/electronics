#include "EEPROM_24XX1025.h"

/*
 * Microchip 24XX1025 I2C EEPROM driver for Arduino
 * Tested with: Arduino Uno R3, 24LC1025 (5 V, 400 kHz I2C)
 * Should work with: all Arduino compatible boards, 24XX1025 models
 *
 * Written by Thomas Backman, August 2012
 *
 * Uses a modified version of Wayne Truchsess' I2C Master library:
 * http://dsscircuits.com/articles/arduino-i2c-master-library.html
 * Changes were made to support 16-bit addresses and acknowledge polling.
 * The unmodified version WILL NOT WORK with this code!
 */

// TODO ITEMS STILL LEFT:
// TODO: final testing, after the code may be 100% finished
// TODO: write a proper README/docs
// TODO: clean up indentation (once again before v1.0 though)

// Finds the block number (0 or 1) from a 17-bit address
#define BLOCKNUM(addr) (( (addr) & (1UL << 16)) >> 16)

// Converts a "full" 17-bit address to the 16-bit page address used by the EEPROM
// The block number (above) is also required, of course, but is sent separately
// in the device address byte.
#define TO_PAGEADDR(addr) ((uint16_t)(addr & 0xffff))

// Undoes the previous two. (Better safe than sorry re: parenthesis and casts, doesn't cost anything!)
#define TO_FULLADDR(block, page) (((uint32_t)(((uint32_t)block) << 16)) | (((uint32_t)(page))))

// Private method
uint8_t EEPROM_24XX1025::readChunk(uint32_t fulladdr, byte *data, uint8_t bytesToRead) {
  if (bytesToRead == 0 || fulladdr > 131071)
    return 0;
  if (fulladdr + bytesToRead > 131071)
    bytesToRead = 131072 - fulladdr;

  uint8_t err = 0;
  if (fulladdr < 65536 && fulladdr + bytesToRead > 65536) {
    // This read crosses the "block boundary" and cannot be sequentially read
    // by the EEPROM itself

    // Read part 1 (from the first block)
    err = I2c16.read(devaddr, fulladdr /* always 16-bit */, 65536 - fulladdr, data);
    if (err) {
      eeprom_pos = 0xffffffff;
      return 0;
    }

    // Read part 2 (from the second block)
    err = I2c16.read(devaddr | (1 << 2), 0, bytesToRead - (65536 - fulladdr), data + (uint16_t)((65536 - fulladdr)));
    if (err) {
      eeprom_pos = 0xffffffff;
      curpos += (65536 - fulladdr); // move the cursor forward the amount we read successfully
      return (uint8_t)(65536 - fulladdr); // num bytes read previously
    }
    else {
      eeprom_pos = TO_FULLADDR(1, bytesToRead - (65536 - fulladdr));
      curpos += bytesToRead;
      return bytesToRead;
    }
  }
  else {
    // Doesn't cross the block border, so we can do this in one read
    uint8_t block = BLOCKNUM(fulladdr);
    err = I2c16.read(devaddr | (block << 2), TO_PAGEADDR(fulladdr), bytesToRead, data);
    if (err) {
      eeprom_pos = 0xffffffff;
      return 0;
    }
    else {
      eeprom_pos += bytesToRead;
      curpos += bytesToRead;
      return bytesToRead;
    }
  }
}

// Private method
uint8_t EEPROM_24XX1025::writeSinglePage(uint32_t fulladdr, byte *data, uint8_t bytesToWrite) {
  // Writes 1 - 128 bytes, but only *within a single page*. Never crosses a page/block border.
  if (bytesToWrite == 0 | bytesToWrite > 128)
    return 0;

  uint8_t ret = I2c16.write(devaddr | ((BLOCKNUM(fulladdr)) << 2), TO_PAGEADDR(fulladdr), data, bytesToWrite);
  if (ret != 0) {
    // We can't be sure what the internal counter is now, since it looks like the write failed.
    eeprom_pos = 0xffffffff;
    return 0;
  }
  else {
    // Try to keep track of the internal counter
    eeprom_pos += bytesToWrite;
    curpos += bytesToWrite;
  }

  // Wait for the EEPROM to finish this write. To do so, we use acknowledge polling,
  // a technique described in the datasheet. We sent a START condition and the device address
  // byte, and see if the device acknowledges (pulls SDA low) or not. Loop until it does.
  while (I2c16.acknowledgePoll(devaddr | ((BLOCKNUM(fulladdr)) << 2)) == 0) {
    delayMicroseconds(20);
  }

  return bytesToWrite;
}

// Private method
uint8_t EEPROM_24XX1025::writeChunk(uint32_t fulladdr, byte *data, uint8_t bytesToWrite) {
  // Used to turn 1-128 byte writes into full page writes (i.e. turn them into proper
  // single-page writes)
  if (bytesToWrite == 0 || bytesToWrite > 128 || fulladdr > 131071)
    return 0;

  uint32_t pageaddr = TO_PAGEADDR(fulladdr);
  uint8_t first_block = BLOCKNUM(fulladdr);
  uint8_t second_block = BLOCKNUM(fulladdr + bytesToWrite - 1);

  // These page numbers are *relative to the block number*, i.e. first_page = 0 may mean at byte 0 or byte 65536
  // depending on first_block above. Same goes for second_page/second_block of course.
  uint16_t first_page = pageaddr / 128; // pageaddr is already relative to block!
  uint16_t second_page = (TO_PAGEADDR(pageaddr + bytesToWrite - 1))/128;

  if (first_page == second_page && first_block == second_block) {
    // Data doesn't "cross the border" between pages. Easy!
    return writeSinglePage(fulladdr, data, bytesToWrite);
  }
  else {
    // The data spans two pages, e.g. begins at address 120 and is 12 bytes long, which would make it go
    // past the edge of this page (addresses 0 - 127) and onto the next.
    // We need to split this write manually.

    uint8_t bytes_in_first_page = ((first_page + 1) * 128) - pageaddr;
    uint8_t bytes_in_second_page = bytesToWrite - bytes_in_first_page;

    uint8_t ret = 0;

    // Write the data that belongs to the first page
    if ((ret = writeSinglePage(TO_FULLADDR(first_block, pageaddr), data, bytes_in_first_page))
      != bytes_in_first_page)
    {
      return ret;
    }

    // Write the data that belongs to the second page
    if ((ret = writeSinglePage(TO_FULLADDR(second_block, second_page * 128), data + bytes_in_first_page, bytes_in_second_page))
      != bytes_in_second_page)
    {
      return ret;
    }
  }

  return bytesToWrite;
}

byte EEPROM_24XX1025::read(void) {
  // Reads a byte from the current position and returns it

  if (eeprom_pos != curpos) {
    // If the EEPROM internal position counter has (or might have) changed,
    // do a "full" read, where we sent the 16-byte address.
    I2c16.read((uint8_t)(devaddr | ((BLOCKNUM(curpos)) << 2)), TO_PAGEADDR(curpos), 1U);
    eeprom_pos = curpos;
  }
  else {
    // If we know that the internal counter is correct, don't send the address, but
    // rely on the EEPROM logic to return the "next" byte properly. This saves
    // overhead and time.
    I2c16.read((uint8_t)(devaddr | ((BLOCKNUM(curpos)) << 2)), 1U);
  }

  curpos++;
  eeprom_pos++;
  if (eeprom_pos == 65536) {
    // Seems to wrap here. The datasheet could be read as if this were 17-bit, but I don't think it is.
    eeprom_pos = 0;
  }
  if (curpos > 131071) {
    // Wrap around if we overflow the device capacity.
    curpos = 0;
    eeprom_pos = 0xffffffff;
  }

  return I2c16.receive(); // Returns 0 if no bytes are queued
}

uint32_t EEPROM_24XX1025::read(byte *data, uint32_t bytesToRead) {
  return read(curpos, data, bytesToRead);
}

uint32_t EEPROM_24XX1025::read(uint32_t fulladdr, byte *data, uint32_t bytesToRead) {
  if (bytesToRead == 0)
    return 0;
  if (bytesToRead <= 255)
    return readChunk(fulladdr, data, bytesToRead); // can be handled without this function
  if (fulladdr + bytesToRead > 131071)
    bytesToRead = 131072 - fulladdr; // constrain read size to end of device

  const uint32_t chunksize = 240; // bytes to read per chunk. Must be smaller than 255.

  // If we get here, we have a >255 byte read that is now constrained to a valid range.
  uint32_t bytesRead = 0;
  uint32_t t = 0;

  while (bytesRead < bytesToRead) {
    t = readChunk(fulladdr + bytesRead, data + bytesRead, min(chunksize, bytesToRead - bytesRead));
    if (t == min(chunksize, bytesToRead - bytesRead))
      bytesRead += t;
    else
      return bytesRead; // Failure!
  }

  return bytesRead;
}

boolean EEPROM_24XX1025::write(byte data) {
  // Writes a byte to the EEPROM.
  // WARNING: writing a single byte still uses a full page write,
  // so writing 128 sequential bytes instead of 1 page write
  // will use 128 times as many of the chip's limited lifetime writes!!
  // In otherwords: ONLY USE THIS if you *really* only need to write ONE byte.
  // Even for just *TWO* bytes, write([pos,] data, bytesToWrite) is "twice as good"!
  // In short: writeBlock for 128 bytes will use 1 page "life" each on 1 or 2 pages.
  // writeByte 128 times will use 128 page "lives", spread over 1 or 2 pages.

  // Find which block the byte is in, based on the full (17-bit) address.
  // We can only supply 16 bits to the EEPROM, plus a separate "block select" bit.
  uint8_t block = BLOCKNUM(curpos);

  uint8_t ret = I2c16.write((uint8_t)(devaddr | (block << 2)), TO_PAGEADDR(curpos), data);
  if (ret != 0) {
    // Looks like something failed. Reset the EEPROM counter "copy", since we're no longer
    // sure what it ACTUALLY is.
    eeprom_pos = 0xffffffff;
    return false;
  }

  curpos++;
  eeprom_pos = curpos; // We changed the internal counter when we wrote the address just above.
  if (curpos > 131071) {
    // The two are equal, no need to check both.
    // Wrap around if we overflow the device capacity.
    curpos = 0;
    eeprom_pos = 0xffffffff; // Not sure what the internal counter does. It PROBABLY resets to 0, but...
  }

  // Wait for the EEPROM to finish this write. To do so, we use acknowledge polling;
  // a a technique described in the datasheet. We sent a START condition and the device address
  // byte, and see if the device acknowledges (pulls SDA low) or not. Loop until it does.
  while (I2c16.acknowledgePoll(devaddr | (block << 2)) == 0) {
    delayMicroseconds(20);
  }

  return true; // success
}

uint32_t EEPROM_24XX1025::write(byte *data, uint32_t bytesToWrite) {
  return write(curpos, data, bytesToWrite);
}

uint32_t EEPROM_24XX1025::write(uint32_t fulladdr, byte *data, uint32_t bytesToWrite) {
  // Uses writeChunk to allow any-sized writes, not just <128 bytes

  if (bytesToWrite == 0)
    return 0;
  if (bytesToWrite <= 128)
    return writeChunk(fulladdr, data, bytesToWrite);
  if (fulladdr + bytesToWrite > 131071)
    bytesToWrite = 131072 - fulladdr; // constrain read size to end of device

  // If we get here, we have a >128 byte write that is now constrained to a valid range.
  uint32_t bytesWritten = 0;
  uint32_t t = 0;

  while (bytesWritten < bytesToWrite) {
    t = writeChunk(fulladdr + bytesWritten, data + bytesWritten, min(128, bytesToWrite - bytesWritten));
    if (t == min(128, bytesToWrite - bytesWritten))
      bytesWritten += t;
    else
      return bytesWritten; //Failure!
  }

  return bytesWritten;
}
