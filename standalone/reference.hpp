#pragma once
#include <cstdint>
namespace qb {
struct FunctionReference { unsigned length; uint64_t hash; };
inline constexpr FunctionReference TickReference{980, 0x4430a26e1c6e6631ULL};
inline constexpr FunctionReference GPoseReference{49, 0xe787d4cea776b2b3ULL};
inline constexpr FunctionReference UpdateReferences[]{{52,0x30fd2cd9a943e0a5ULL},{53,0xbfd78b75ac17498eULL},{53,0xbfd78b75ac17498eULL},{1104,0x7c2a6c43166811fULL}};
}
