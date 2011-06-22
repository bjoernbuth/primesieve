/*
 * PrimeNumberFinder.cpp -- This file is part of primesieve
 *
 * Copyright (C) 2011 Kim Walisch, <kim.walisch@gmail.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>
 */

#include "PrimeNumberFinder.h"
#include "PrimeSieve.h"
#include "SieveOfEratosthenes.h"
#include "defs.h"
#include "cpuid.h" 
#include "pmath.h"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <cassert>
#include <sstream>

namespace {
  const uint32_t BYTE_SIZE = (1 << 8);
  const uint32_t END = BYTE_SIZE;
}

const uint32_t PrimeNumberFinder::nextBitValue_[NUMBERS_PER_BYTE] = { 0,
     0, 0, 0, 0,  0, 0,
    11, 0, 0, 0, 13, 0,
    17, 0, 0, 0, 19, 0,
    23, 0, 0, 0, 29, 0,
     0, 0, 0, 0, 31 };

PrimeNumberFinder::PrimeNumberFinder(PrimeSieve* ps) :
  SieveOfEratosthenes(
      std::max<uint64_t>(ps->getStartNumber(), 7),
      ps->getStopNumber(),
      ps->getSieveSize() * 1024,
      ps->getPreSieveLimit()),
      primeSieve_(ps), primeByteCounts_(NULL), primeBitValues_(NULL) {
  if (isPOPCNTSupported())
    primeSieve_->flags_ |= PrimeSieve::SSE4_POPCNT;
  this->initLookupTables();
}

PrimeNumberFinder::~PrimeNumberFinder() {
  if (primeByteCounts_ != NULL) {
    for (uint32_t i = 0; i < COUNTS_SIZE; i++)
      delete[] primeByteCounts_[i];
    delete[] primeByteCounts_;
  }
  if (primeBitValues_ != NULL) {
    for (uint32_t i = 0; i < BYTE_SIZE; i++)
      delete[] primeBitValues_[i];
    delete[] primeBitValues_;
  }
}

void PrimeNumberFinder::initLookupTables() {
  const uint32_t bitmasks[COUNTS_SIZE][9] = {{ 0x01, 0x02, 0x04, 0x08,
        0x10, 0x20, 0x40, 0x80, END }, // prime number bits
      { 0x06, 0x18, 0xc0, END },       // twin prime bitmasks
      { 0x07, 0x0e, 0x1c, 0x38, END }, // prime triplet bitmasks
      { 0x1e, END },                   // prime quadruplet bitmasks
      { 0x1f, 0x3e, END },             // prime quintuplet bitmasks
      { 0x3f, END },                   // prime sextuplet bitmasks     
      { 0xfe, END } };                 // prime septuplet bitmasks

  // initialize the primeByteCounts_ lookup tables needed to count the
  // prime numbers and prime k-tuplets per byte
  if (primeSieve_->flags_ & PrimeSieve::COUNT_FLAGS) {
    primeByteCounts_ = new uint32_t*[COUNTS_SIZE];
    for (uint32_t i = 0; i < COUNTS_SIZE; i++) {
      primeByteCounts_[i] = NULL;
      if (primeSieve_->flags_  & (PrimeSieve::COUNT_PRIMES << i)) {
        primeByteCounts_[i] = new uint32_t[BYTE_SIZE];
        for (uint32_t j = 0; j < BYTE_SIZE; j++) {
          uint32_t bitmaskCount = 0;
          for (const uint32_t* b = bitmasks[i]; *b <= j; b++) {
            if ((j & *b) == *b)
              bitmaskCount++;
          }
          primeByteCounts_[i][j] = bitmaskCount;
        }
      }
    }
  }
  // initialize the primeBitValues_ lookup tables needed to
  // reconstruct prime numbers and prime k-tuplets from 1 bits of the
  // sieve array
  if (primeSieve_->flags_ & PrimeSieve::GENERATE_FLAGS) {
    primeBitValues_ = new uint32_t*[BYTE_SIZE];
    uint32_t generateType = 0;
    if (primeSieve_->flags_ & PrimeSieve::PRINT_FLAGS)
      for (uint32_t i = PrimeSieve::PRINT_PRIMES; (i & primeSieve_->flags_) == 0; i <<= 1)
        generateType++;
    for (uint32_t i = 0; i < BYTE_SIZE; i++) {
      primeBitValues_[i] = new uint32_t[9];
      uint32_t bitmaskCount = 0;
      for (const uint32_t* b = bitmasks[generateType]; *b <= i; b++) {
        if ((i & *b) == *b)
          primeBitValues_[i][bitmaskCount++] = bitValues_[ntz(*b)];
      }
      primeBitValues_[i][bitmaskCount] = END;
    }
  }
}

/**
 * Count the prime numbers and prime k-tuplets within the current
 * segment.
 */
void PrimeNumberFinder::count(const uint8_t* sieve, uint32_t sieveSize) {
  // count prime numbers
  if (primeSieve_->flags_ & PrimeSieve::COUNT_PRIMES) {
    uint32_t primeCount = 0;
    uint32_t i = 0;
#if defined(POPCNT64)
    // count bits using the SSE 4.2 POPCNT instruction
    if (primeSieve_->flags_ & PrimeSieve::SSE4_POPCNT)
      for (; i + 8 < sieveSize; i += 8)
        primeCount += POPCNT64(sieve, i);
#endif
    // count bits using a lookup table
    for (; i < sieveSize; i++)
      primeCount += primeByteCounts_[0][sieve[i]];
    primeSieve_->counts_[0] += primeCount;
  }
  // count prime k-tuplets (twins, triplets, ...)
  for (uint32_t i = 1; i < COUNTS_SIZE; i++) {
    if (primeSieve_->flags_ & (PrimeSieve::COUNT_PRIMES << i)) {
      uint32_t kTupletCount = 0;
      for (uint32_t j = 0; j < sieveSize; j++) {
        kTupletCount += primeByteCounts_[i][sieve[j]];
      }
      primeSieve_->counts_[i] += kTupletCount;
    }
  }
}

/**
 * Reconstruct prime numbers or prime k-tuplets (twin primes, prime
 * triplets, ...) from 1 bits of the sieve array.
 */
void PrimeNumberFinder::generate(const uint8_t* sieve, uint32_t sieveSize) {
  uint64_t byteValue = this->getSegmentLow();

  // call a callback function for each prime number
  if (primeSieve_->flags_ & PrimeSieve::CALLBACK_PRIMES)
    for (uint32_t i = 0; i < sieveSize; i++, byteValue += NUMBERS_PER_BYTE)
      for (uint32_t* bitValue = primeBitValues_[sieve[i]]; *bitValue != END; bitValue++) {
        uint64_t prime = byteValue + *bitValue;
        primeSieve_->callback_(prime);
      }
  // call an OOP style callback function for each prime number
  else if (primeSieve_->flags_ & PrimeSieve::CALLBACK_PRIMES_OOP)
    for (uint32_t i = 0; i < sieveSize; i++, byteValue += NUMBERS_PER_BYTE)
      for (uint32_t* bitValue = primeBitValues_[sieve[i]]; *bitValue != END; bitValue++) {
        uint64_t prime = byteValue + *bitValue;
        primeSieve_->callbackOOP_(prime, primeSieve_->cbObj_);
      }
  // print the prime numbers to stdout
  else if (primeSieve_->flags_ & PrimeSieve::PRINT_PRIMES)
    for (uint32_t i = 0; i < sieveSize; i++, byteValue += NUMBERS_PER_BYTE)
      for (uint32_t* bitValue = primeBitValues_[sieve[i]]; *bitValue != END; bitValue++) {
        std::ostringstream prime;
        prime << byteValue + *bitValue << '\n';
        std::cout << prime.str();
      }
  // print the prime k-tuplets to stdout
  else {
    for (uint32_t i = 0; i < sieveSize; i++, byteValue += NUMBERS_PER_BYTE) {
      for (uint32_t* bitValue = primeBitValues_[sieve[i]]; *bitValue != END; bitValue++) {
        std::ostringstream ktuplet;
        ktuplet << '(';
        uint32_t v = *bitValue;
        for (uint32_t j = PrimeSieve::PRINT_PRIMES; 
            (j & primeSieve_->flags_) == 0; j <<= 1) {
          ktuplet << byteValue + v << ", ";
          v = nextBitValue_[v];
        }
        ktuplet << byteValue + v << ")\n";
        std::cout << ktuplet.str();
      }
    }
  }
}

void PrimeNumberFinder::analyseSieve(const uint8_t* sieve, uint32_t sieveSize) {
  if (primeSieve_->flags_ & PrimeSieve::COUNT_FLAGS)
    this->count(sieve, sieveSize);
  if (primeSieve_->flags_ & PrimeSieve::GENERATE_FLAGS)
    this->generate(sieve, sieveSize);
  primeSieve_->parent_->doStatus(sieveSize * NUMBERS_PER_BYTE);
}
