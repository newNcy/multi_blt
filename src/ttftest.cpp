// 测试 FA 字体 cmap 是否映射指定码位（与 ImGui 同款解析器 stb_truetype）
#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char** argv)
{
    const char* path = argc > 1 ? argv[1] : "G:\\multi_blt\\build\\fa-solid-900.ttf";
    FILE* f = fopen(path, "rb");
    if (!f) { printf("cannot open %s\n", path); return 1; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char* buf = (unsigned char*)malloc(sz);
    if (!buf || fread(buf, 1, sz, f) != (size_t)sz) { printf("read fail\n"); return 1; }
    fclose(f);

    stbtt_fontinfo fi;
    if (!stbtt_InitFont(&fi, buf, 0)) { printf("stbtt_InitFont FAILED (not a valid ttf?)\n"); return 1; }
    printf("font ok, size=%ld\n", sz);

    int cps[] = { 0xf027, 0xf028, 0xf1e6, 0xf287, 0xf025, 0xf6a9, 0xf00d, 0 };
    for (int i = 0; cps[i]; ++i)
        printf("U+%04X gid=%d %s\n", cps[i], stbtt_FindGlyphIndex(&fi, cps[i]),
               stbtt_FindGlyphIndex(&fi, cps[i]) ? "" : "<-- MISSING");
    return 0;
}
