/*
 * stb JPEG 解码器的实现单元。
 *
 * 单独成文件是因为 stb 的用法要求：STB_IMAGE_IMPLEMENTATION 必须
 * 且只能在一个编译单元里出现一次。这样 cam_view.c 只看到声明，
 * 编译速度也快一些。
 *
 * stb_image 是公有领域 / MIT 双许可，版权归 Sean Barrett。
 */

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO        /* 我们只用内存解码，禁用文件 I/O，省体积 */
#define STBI_NO_PSD
#define STBI_NO_PIC
#define STBI_NO_PNM
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STBI_ONLY_JPEG       /* 只需要 JPEG */

#include "third_party/stb_image.h"
