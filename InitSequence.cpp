#include "InitSequence.h"

#include <cctype>
#include <cstdlib>

namespace DekiDisplay
{

InitSequence InitSequenceFromTable(const uint8_t* table)
{
    constexpr uint8_t kDelayFollows = 0x80;
    InitSequence out;
    if (!table)
    {
        return out;
    }
    for (;;)
    {
        const uint8_t cmd = *table++;
        const uint8_t num = *table++;
        if (cmd == 0xFF && num == 0xFF)
        {
            break;
        }
        InitStep step;
        step.cmd = cmd;
        const uint8_t count = num & static_cast<uint8_t>(~kDelayFollows);
        step.params.assign(table, table + count);
        table += count;
        if (num & kDelayFollows)
        {
            const uint8_t ms = *table++;
            step.delayMs = ms == 255 ? 500 : ms;
        }
        out.push_back(std::move(step));
    }
    return out;
}

namespace
{

bool ParseHexByte(const std::string& token, uint8_t& value)
{
    std::string digits = token;
    if (digits.size() > 2 && digits[0] == '0' && (digits[1] == 'x' || digits[1] == 'X'))
    {
        digits = digits.substr(2);
    }
    if (digits.empty() || digits.size() > 2)
    {
        return false;
    }
    for (char c : digits)
    {
        if (!std::isxdigit(static_cast<unsigned char>(c)))
        {
            return false;
        }
    }
    value = static_cast<uint8_t>(std::strtoul(digits.c_str(), nullptr, 16));
    return true;
}

bool ParseMs(const std::string& token, uint32_t& value)
{
    if (token.empty() || token.size() > 5)
    {
        return false;
    }
    for (char c : token)
    {
        if (!std::isdigit(static_cast<unsigned char>(c)))
        {
            return false;
        }
    }
    value = static_cast<uint32_t>(std::strtoul(token.c_str(), nullptr, 10));
    return true;
}

std::vector<std::string> Tokens(const std::string& item)
{
    std::vector<std::string> out;
    std::string cur;
    for (char c : item)
    {
        if (std::isspace(static_cast<unsigned char>(c)) || c == ',')
        {
            if (!cur.empty())
            {
                out.push_back(cur);
                cur.clear();
            }
        }
        else
        {
            cur += c;
        }
    }
    if (!cur.empty())
    {
        out.push_back(cur);
    }
    return out;
}

bool IsDelay(const std::string& token)
{
    if (token.size() != 5)
    {
        return false;
    }
    std::string lower;
    for (char c : token)
    {
        lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return lower == "delay";
}

}  // namespace

bool ParseInitSequence(const std::string& text, InitSequence& out, std::string& error)
{
    out.clear();
    error.clear();
    int lineNo = 0;
    size_t start = 0;
    while (start <= text.size())
    {
        size_t end = text.find('\n', start);
        if (end == std::string::npos)
        {
            end = text.size();
        }
        std::string line = text.substr(start, end - start);
        start = end + 1;
        ++lineNo;

        const size_t hash = line.find('#');
        if (hash != std::string::npos)
        {
            line.resize(hash);
        }

        size_t itemStart = 0;
        while (itemStart <= line.size())
        {
            size_t itemEnd = line.find(';', itemStart);
            if (itemEnd == std::string::npos)
            {
                itemEnd = line.size();
            }
            const std::vector<std::string> tokens = Tokens(line.substr(itemStart, itemEnd - itemStart));
            itemStart = itemEnd + 1;
            if (tokens.empty())
            {
                continue;
            }

            size_t i = 0;
            if (!IsDelay(tokens[0]))
            {
                InitStep step;
                if (!ParseHexByte(tokens[0], step.cmd))
                {
                    error = "line " + std::to_string(lineNo) + ": '" + tokens[0] + "' is not a hex command byte";
                    return false;
                }
                for (i = 1; i < tokens.size() && !IsDelay(tokens[i]); ++i)
                {
                    uint8_t b = 0;
                    if (!ParseHexByte(tokens[i], b))
                    {
                        error = "line " + std::to_string(lineNo) + ": '" + tokens[i] + "' is not a hex byte";
                        return false;
                    }
                    step.params.push_back(b);
                }
                out.push_back(std::move(step));
            }
            if (i < tokens.size())
            {
                // "delay <ms>", for the command before it.
                if (out.empty())
                {
                    error = "line " + std::to_string(lineNo) + ": a delay needs a command before it";
                    return false;
                }
                uint32_t ms = 0;
                if (i + 1 >= tokens.size() || !ParseMs(tokens[i + 1], ms))
                {
                    error = "line " + std::to_string(lineNo) + ": 'delay' takes a number of milliseconds";
                    return false;
                }
                if (i + 2 < tokens.size())
                {
                    error = "line " + std::to_string(lineNo) + ": '" + tokens[i + 2] +
                            "' after a delay; start the next command with ';' or a new line";
                    return false;
                }
                out.back().delayMs += ms;
            }
        }
    }
    return true;
}

}  // namespace DekiDisplay
