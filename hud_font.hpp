#pragma once

#include <cstdint>

namespace fod::hud {

inline const uint8_t* glyph(char c) {
    static constexpr uint8_t alphabet[][7] = {
        {14,17,17,31,17,17,17},{30,17,17,30,17,17,30},{14,17,16,16,16,17,14},
        {30,17,17,17,17,17,30},{31,16,16,30,16,16,31},{31,16,16,30,16,16,16},
        {14,17,16,23,17,17,15},{17,17,17,31,17,17,17},{14,4,4,4,4,4,14},
        {7,2,2,2,2,18,12},{17,18,20,24,20,18,17},{16,16,16,16,16,16,31},
        {17,27,21,21,17,17,17},{17,25,21,19,17,17,17},{14,17,17,17,17,17,14},
        {30,17,17,30,16,16,16},{14,17,17,17,21,18,13},{30,17,17,30,20,18,17},
        {15,16,16,14,1,1,30},{31,4,4,4,4,4,4},{17,17,17,17,17,17,14},
        {17,17,17,17,17,10,4},{17,17,17,21,21,21,10},{17,17,10,4,10,17,17},
        {17,17,10,4,4,4,4},{31,1,2,4,8,16,31}
    };
    static constexpr uint8_t numbers[][7] = {
        {14,17,19,21,25,17,14},{4,12,4,4,4,4,14},{14,17,1,2,4,8,31},
        {30,1,1,14,1,1,30},{2,6,10,18,31,2,2},{31,16,16,30,1,1,30},
        {14,16,16,30,17,17,14},{31,1,2,4,8,8,8},{14,17,17,14,17,17,14},
        {14,17,17,15,1,1,14}
    };
    static constexpr uint8_t space[7]{}, slash[7]{1,1,2,4,8,16,16}, minus[7]{0,0,0,31,0,0,0};
    static constexpr uint8_t plus[7]{0,4,4,31,4,4,0}, dot[7]{0,0,0,0,0,12,12};
    static constexpr uint8_t colon[7]{0,12,12,0,12,12,0}, underscore[7]{0,0,0,0,0,0,31};
    static constexpr uint8_t left[7]{0,2,4,8,4,2,0}, right[7]{0,8,4,2,4,8,0};
    static constexpr uint8_t percent[7]{25,25,2,4,8,19,19};
    static constexpr uint8_t bang[7]{4,4,4,4,4,0,4}, comma[7]{0,0,0,0,4,4,8};
    static constexpr uint8_t apostrophe[7]{4,4,8,0,0,0,0}, question[7]{14,17,1,6,4,0,4};
    static constexpr uint8_t lparen[7]{2,4,8,8,8,4,2}, rparen[7]{8,4,2,2,2,4,8};
    static constexpr uint8_t ampersand[7]{12,18,20,8,21,18,13};
    if (c >= 'a' && c <= 'z') c = char(c - 'a' + 'A');
    if (c >= 'A' && c <= 'Z') return alphabet[c - 'A'];
    if (c >= '0' && c <= '9') return numbers[c - '0'];
    switch (c) {
    case '/': return slash;
    case '-': return minus;
    case '+': return plus;
    case '.': return dot;
    case ':': return colon;
    case '_': return underscore;
    case '<': return left;
    case '>': return right;
    case '%': return percent;
    case '!': return bang;
    case ',': return comma;
    case '\'': return apostrophe;
    case '?': return question;
    case '(': return lparen;
    case ')': return rparen;
    case '&': return ampersand;
    default: return space;
    }
}

} // namespace fod::hud
