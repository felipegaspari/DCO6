#ifndef __UTILS_H__
#define __UTILS_H__

uint16_t linearToLogarithmic(uint16_t linearValue, float base, uint16_t maxValue);
uint16_t linearToExponential(uint16_t linearValue, float base, uint16_t maxValue);
float expConverterFloat(uint16_t readingValue, uint16_t curve);
uint16_t expConverter(uint16_t readingValue, uint16_t curve);

#endif
