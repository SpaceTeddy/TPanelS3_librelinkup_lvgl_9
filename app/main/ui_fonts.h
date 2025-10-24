/**
 * @file ui_fonts.h
 * @brief Custom font declarations for ESP32 LibreLinkUp Client UI
 * @version 1.0
 * @date 2025
 * 
 * This header declares all custom JetBrains Mono Light fonts used
 * throughout the user interface. These fonts must be generated and
 * included in the project before compilation.
 * 
 * @note All fonts are based on JetBrains Mono Light typeface
 * @note Font files should be generated using LVGL Font Converter
 * @note Project: ESP32 LibreLinkUp Client
 */

#ifndef UI_FONTS_H
#define UI_FONTS_H

#ifdef __cplusplus
extern "C" {
#endif

///////////////////// INCLUDES ////////////////////

#include "lvgl.h"

///////////////////// FONT DECLARATIONS ////////////////////

/**
 * @defgroup ui_fonts Custom UI Fonts
 * @brief JetBrains Mono Light font family in various sizes
 * 
 * All fonts are monospace and use the Light weight variant of
 * JetBrains Mono. Sizes are optimized for a 480x480 display.
 * 
 * @note These fonts must be generated using LVGL online font converter
 *       at https://lvgl.io/tools/fontconverter
 * @note Recommended settings:
 *       - Font: JetBrains Mono Light
 *       - BPP: 4 bit-per-pixel
 *       - Range: 0x20-0x7F (Basic Latin) + custom symbols
 *       - Format: C array (*.c)
 * @{
 */

/**
 * @brief 16px JetBrains Mono Light font
 * 
 * Used for:
 * - Small labels and descriptions
 * - Chart axis labels
 * - Sensor validity progress bar labels
 * - Secondary information text
 * 
 * @note Size: 16 pixels
 */
extern lv_font_t JetBrainsMonoLight16;

/**
 * @brief 20px JetBrains Mono Light font
 * 
 * Used for:
 * - Debug screen information
 * - System status displays (IP, time, sensor data)
 * - Detailed text information
 * 
 * @note Size: 20 pixels
 */
extern lv_font_t JetBrainsMonoLight20;

/**
 * @brief 24px JetBrains Mono Light font
 * 
 * Used for:
 * - Welcome screen WiFi information
 * - Secondary status indicators
 * - Medium-emphasis text
 * 
 * @note Size: 24 pixels
 */
extern lv_font_t JetBrainsMonoLight24;

/**
 * @brief 32px JetBrains Mono Light font
 * 
 * Used for:
 * - Screen titles (Debug, Login)
 * - Section headers
 * - Important status messages
 * 
 * @note Size: 32 pixels
 */
extern lv_font_t JetBrainsMonoLight32;

/**
 * @brief 36px JetBrains Mono Light font
 * 
 * Used for:
 * - Glucose delta values (change indicators)
 * - Alert and warning messages
 * - API activity indicators
 * - Firmware update status
 * 
 * @note Size: 36 pixels
 */
extern lv_font_t JetBrainsMonoLight36;

/**
 * @brief 56px JetBrains Mono Light font
 * 
 * Used for:
 * - Welcome screen main title
 * - Large emphasis text
 * 
 * @note Size: 56 pixels
 */
extern lv_font_t JetBrainsMonoLight56;

/**
 * @brief 72px JetBrains Mono Light font
 * 
 * Used for:
 * - Glucose trend arrows (↑ ↗ → ↘ ↓)
 * - Large symbols and icons
 * - Firmware update progress percentage
 * 
 * @note Size: 72 pixels
 * @note Must include Unicode arrow symbols:
 *       - U+2191 (↑) Upward arrow
 *       - U+2197 (↗) Northeast arrow  
 *       - U+2192 (→) Right arrow
 *       - U+2198 (↘) Southeast arrow
 *       - U+2193 (↓) Downward arrow
 */
extern lv_font_t JetBrainsMonoLight72;

/**
 * @brief 100px JetBrains Mono Light font
 * 
 * Used for:
 * - Main glucose value display (primary reading)
 * - Most prominent UI element
 * 
 * @note Size: 100 pixels
 * @note This is the largest font in the UI
 * @warning Requires significant memory (~10-15KB per font)
 */
extern lv_font_t JetBrainsMonoLight100;

/** @} */ // end of ui_fonts

///////////////////// FONT GENERATION GUIDE ////////////////////

/**
 * @page font_generation Font Generation Guide
 * 
 * @section generation_steps How to Generate Fonts
 * 
 * @subsection step1 Step 1: Access LVGL Font Converter
 * Navigate to: https://lvgl.io/tools/fontconverter
 * 
 * @subsection step2 Step 2: Configure Font Settings
 * For each font size (16, 20, 24, 32, 36, 56, 72, 100):
 * 
 * 1. **Name**: JetBrainsMonoLight[SIZE] (e.g., JetBrainsMonoLight16)
 * 2. **Size**: Enter the pixel height
 * 3. **BPP**: Select "4 bit-per-pixel"
 * 4. **TTF/WOFF Font**: Upload JetBrains Mono Light (.ttf)
 * 5. **Range**: Add the following ranges:
 *    - Basic Latin: 0x20-0x7F
 *    - Latin-1 Supplement: 0xA0-0xFF (for special characters)
 *    - Custom symbols (if needed)
 * 6. **Symbols**: For JetBrainsMonoLight72, add:
 *    - ↑ (U+2191)
 *    - ↗ (U+2197)
 *    - → (U+2192)
 *    - ↘ (U+2198)
 *    - ↓ (U+2193)
 * 7. **Format**: Select "C array (*.c)"
 * 8. **Try it**: Preview the font
 * 9. **Download**: Click "Convert" and download
 * 
 * @subsection step3 Step 3: Integration
 * 
 * 1. Place generated .c files in your project's fonts directory
 * 2. Include them in your build system (CMakeLists.txt or Makefile)
 * 3. The fonts are automatically available through this header
 * 
 * @subsection step4 Step 4: CMakeLists.txt Example
 * @code
 * # Add font source files
 * set(FONT_SOURCES
 *     fonts/JetBrainsMonoLight16.c
 *     fonts/JetBrainsMonoLight20.c
 *     fonts/JetBrainsMonoLight24.c
 *     fonts/JetBrainsMonoLight32.c
 *     fonts/JetBrainsMonoLight36.c
 *     fonts/JetBrainsMonoLight56.c
 *     fonts/JetBrainsMonoLight72.c
 *     fonts/JetBrainsMonoLight100.c
 * )
 * 
 * # Add to your component
 * idf_component_register(
 *     SRCS ${FONT_SOURCES} ui.cpp
 *     INCLUDE_DIRS .
 * )
 * @endcode
 * 
 * @section memory_usage Memory Usage Estimates
 * 
 * Approximate memory requirements per font (4 BPP, ASCII range):
 * - JetBrainsMonoLight16:  ~3-4 KB
 * - JetBrainsMonoLight20:  ~4-5 KB
 * - JetBrainsMonoLight24:  ~5-6 KB
 * - JetBrainsMonoLight32:  ~7-8 KB
 * - JetBrainsMonoLight36:  ~8-9 KB
 * - JetBrainsMonoLight56:  ~10-12 KB
 * - JetBrainsMonoLight72:  ~12-14 KB
 * - JetBrainsMonoLight100: ~15-18 KB
 * 
 * **Total estimated memory: ~70-80 KB**
 * 
 * @note Actual size depends on character range and BPP settings
 * @note Consider using compressed fonts for memory-constrained systems
 * 
 * @section troubleshooting Troubleshooting
 * 
 * @subsection missing_symbols Missing Symbols
 * If symbols don't display:
 * 1. Verify the Unicode range includes the symbol
 * 2. Check that the TTF font file contains the glyph
 * 3. Try using "Symbols" list instead of range
 * 
 * @subsection wrong_size Wrong Font Size
 * If fonts appear too large/small:
 * 1. Verify lv_conf.h has correct DPI settings
 * 2. Check display driver resolution matches physical display
 * 3. Adjust font size in generator and regenerate
 * 
 * @subsection compile_errors Compilation Errors
 * If fonts don't compile:
 * 1. Ensure .c files are in build system
 * 2. Check for naming conflicts (must match extern declarations)
 * 3. Verify LVGL version compatibility (fonts are version-specific)
 */

/**
 * @page font_download Font Download
 * 
 * @section download_jetbrains Download JetBrains Mono
 * 
 * The JetBrains Mono font family can be downloaded from:
 * 
 * **Official Website:**
 * https://www.jetbrains.com/lp/mono/
 * 
 * **GitHub Repository:**
 * https://github.com/JetBrains/JetBrainsMono
 * 
 * **Direct Download (Latest Release):**
 * https://github.com/JetBrains/JetBrainsMono/releases
 * 
 * @subsection required_file Required File
 * After downloading, extract and locate:
 * - **File**: JetBrainsMono-Light.ttf
 * - **Path**: fonts/ttf/JetBrainsMono-Light.ttf
 * 
 * @subsection license License Information
 * JetBrains Mono is licensed under the OFL-1.1 (SIL Open Font License).
 * This allows free use in both commercial and non-commercial projects.
 * 
 * @note Always verify license compatibility with your project requirements
 * @note Include license file when distributing fonts
 */

/**
 * @page font_alternatives Alternative Fonts
 * 
 * @section alternatives If JetBrains Mono is unavailable
 * 
 * Compatible monospace alternatives:
 * 
 * 1. **Roboto Mono** (Google Fonts)
 *    - Similar metrics to JetBrains Mono
 *    - Excellent readability
 *    - Freely available
 * 
 * 2. **Fira Code** (Mozilla)
 *    - Includes programming ligatures
 *    - Clean and modern
 *    - Open source
 * 
 * 3. **Source Code Pro** (Adobe)
 *    - Designed for coding
 *    - Good Unicode coverage
 *    - SIL Open Font License
 * 
 * 4. **Ubuntu Mono** (Canonical)
 *    - Clean and readable
 *    - Good for displays
 *    - Ubuntu Font License
 * 
 * @note When changing fonts, regenerate all sizes with new TTF file
 * @note Update font names in declarations to match new font family
 */

#ifdef __cplusplus
} // extern "C"
#endif

#endif // UI_FONTS_H