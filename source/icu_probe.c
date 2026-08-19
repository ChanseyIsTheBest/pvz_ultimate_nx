/* icu_probe.c -- find out which ICU functions the game ACTUALLY calls.
 *
 * The enumeration run answered the first question: the runtime resolves the
 * whole System.Globalization.Native surface, roughly 110 entries spanning
 * collation, search, normalisation, calendars, formatting, locale data and
 * resource bundles. Taken at face value that rules out a shim.
 *
 * But pal_icushim resolves every one of those EAGERLY at load, whether or not
 * a single one is ever invoked. What decides the strategy is not how many are
 * looked up -- it is how many are called, and that is usually a far smaller
 * set. A game whose text handling is ASCII may touch only casing and
 * comparison.
 *
 * So every symbol now resolves to its OWN trampoline rather than a shared
 * trap, which means a call can be attributed to a specific function. Calls are
 * logged once each and return zero so execution continues as far as it can --
 * one run yields many answers instead of one. Zero is not a correct ICU
 * return, so a crash after a few calls is expected; the log up to that point
 * is the deliverable.
 *
 * Read the [icu] CALLED lines in the next log:
 *   a handful, mostly u_tolower / u_toupper / u_strcmp   -> a shim is viable
 *   collators, normalisers, resource bundles in anger    -> port real ICU
 *
 * This file is scaffolding. Delete it once the decision is made -- it cannot
 * make the game correct, only tell us what correctness would require.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "icu_probe.h"
#include "util.h"

/* Any version inside the range the runtime scans; it only wants to know which
 * one it is talking to. */
#define PRETEND_ICU_VERSION 68
#define MAX_ICU_SYMBOLS 192

static char g_names[MAX_ICU_SYMBOLS][64];
static unsigned char g_reported[MAX_ICU_SYMBOLS];
static int g_count;
static int g_calls;

static intptr_t icu_called(int idx) {
  g_calls++;
  if (idx >= 0 && idx < MAX_ICU_SYMBOLS && !g_reported[idx]) {
    g_reported[idx] = 1;
    debug_log("[icu] CALLED %s\n", g_names[idx]);
  }
  /* Zero, deliberately: NULL for handles, U_ZERO_ERROR-ish for codes. It is
   * wrong, and it will eventually fault -- but it keeps execution moving long
   * enough to observe several calls rather than exactly one. */
  return 0;
}

static intptr_t icu_t0(void) { return icu_called(0); }
static intptr_t icu_t1(void) { return icu_called(1); }
static intptr_t icu_t2(void) { return icu_called(2); }
static intptr_t icu_t3(void) { return icu_called(3); }
static intptr_t icu_t4(void) { return icu_called(4); }
static intptr_t icu_t5(void) { return icu_called(5); }
static intptr_t icu_t6(void) { return icu_called(6); }
static intptr_t icu_t7(void) { return icu_called(7); }
static intptr_t icu_t8(void) { return icu_called(8); }
static intptr_t icu_t9(void) { return icu_called(9); }
static intptr_t icu_t10(void) { return icu_called(10); }
static intptr_t icu_t11(void) { return icu_called(11); }
static intptr_t icu_t12(void) { return icu_called(12); }
static intptr_t icu_t13(void) { return icu_called(13); }
static intptr_t icu_t14(void) { return icu_called(14); }
static intptr_t icu_t15(void) { return icu_called(15); }
static intptr_t icu_t16(void) { return icu_called(16); }
static intptr_t icu_t17(void) { return icu_called(17); }
static intptr_t icu_t18(void) { return icu_called(18); }
static intptr_t icu_t19(void) { return icu_called(19); }
static intptr_t icu_t20(void) { return icu_called(20); }
static intptr_t icu_t21(void) { return icu_called(21); }
static intptr_t icu_t22(void) { return icu_called(22); }
static intptr_t icu_t23(void) { return icu_called(23); }
static intptr_t icu_t24(void) { return icu_called(24); }
static intptr_t icu_t25(void) { return icu_called(25); }
static intptr_t icu_t26(void) { return icu_called(26); }
static intptr_t icu_t27(void) { return icu_called(27); }
static intptr_t icu_t28(void) { return icu_called(28); }
static intptr_t icu_t29(void) { return icu_called(29); }
static intptr_t icu_t30(void) { return icu_called(30); }
static intptr_t icu_t31(void) { return icu_called(31); }
static intptr_t icu_t32(void) { return icu_called(32); }
static intptr_t icu_t33(void) { return icu_called(33); }
static intptr_t icu_t34(void) { return icu_called(34); }
static intptr_t icu_t35(void) { return icu_called(35); }
static intptr_t icu_t36(void) { return icu_called(36); }
static intptr_t icu_t37(void) { return icu_called(37); }
static intptr_t icu_t38(void) { return icu_called(38); }
static intptr_t icu_t39(void) { return icu_called(39); }
static intptr_t icu_t40(void) { return icu_called(40); }
static intptr_t icu_t41(void) { return icu_called(41); }
static intptr_t icu_t42(void) { return icu_called(42); }
static intptr_t icu_t43(void) { return icu_called(43); }
static intptr_t icu_t44(void) { return icu_called(44); }
static intptr_t icu_t45(void) { return icu_called(45); }
static intptr_t icu_t46(void) { return icu_called(46); }
static intptr_t icu_t47(void) { return icu_called(47); }
static intptr_t icu_t48(void) { return icu_called(48); }
static intptr_t icu_t49(void) { return icu_called(49); }
static intptr_t icu_t50(void) { return icu_called(50); }
static intptr_t icu_t51(void) { return icu_called(51); }
static intptr_t icu_t52(void) { return icu_called(52); }
static intptr_t icu_t53(void) { return icu_called(53); }
static intptr_t icu_t54(void) { return icu_called(54); }
static intptr_t icu_t55(void) { return icu_called(55); }
static intptr_t icu_t56(void) { return icu_called(56); }
static intptr_t icu_t57(void) { return icu_called(57); }
static intptr_t icu_t58(void) { return icu_called(58); }
static intptr_t icu_t59(void) { return icu_called(59); }
static intptr_t icu_t60(void) { return icu_called(60); }
static intptr_t icu_t61(void) { return icu_called(61); }
static intptr_t icu_t62(void) { return icu_called(62); }
static intptr_t icu_t63(void) { return icu_called(63); }
static intptr_t icu_t64(void) { return icu_called(64); }
static intptr_t icu_t65(void) { return icu_called(65); }
static intptr_t icu_t66(void) { return icu_called(66); }
static intptr_t icu_t67(void) { return icu_called(67); }
static intptr_t icu_t68(void) { return icu_called(68); }
static intptr_t icu_t69(void) { return icu_called(69); }
static intptr_t icu_t70(void) { return icu_called(70); }
static intptr_t icu_t71(void) { return icu_called(71); }
static intptr_t icu_t72(void) { return icu_called(72); }
static intptr_t icu_t73(void) { return icu_called(73); }
static intptr_t icu_t74(void) { return icu_called(74); }
static intptr_t icu_t75(void) { return icu_called(75); }
static intptr_t icu_t76(void) { return icu_called(76); }
static intptr_t icu_t77(void) { return icu_called(77); }
static intptr_t icu_t78(void) { return icu_called(78); }
static intptr_t icu_t79(void) { return icu_called(79); }
static intptr_t icu_t80(void) { return icu_called(80); }
static intptr_t icu_t81(void) { return icu_called(81); }
static intptr_t icu_t82(void) { return icu_called(82); }
static intptr_t icu_t83(void) { return icu_called(83); }
static intptr_t icu_t84(void) { return icu_called(84); }
static intptr_t icu_t85(void) { return icu_called(85); }
static intptr_t icu_t86(void) { return icu_called(86); }
static intptr_t icu_t87(void) { return icu_called(87); }
static intptr_t icu_t88(void) { return icu_called(88); }
static intptr_t icu_t89(void) { return icu_called(89); }
static intptr_t icu_t90(void) { return icu_called(90); }
static intptr_t icu_t91(void) { return icu_called(91); }
static intptr_t icu_t92(void) { return icu_called(92); }
static intptr_t icu_t93(void) { return icu_called(93); }
static intptr_t icu_t94(void) { return icu_called(94); }
static intptr_t icu_t95(void) { return icu_called(95); }
static intptr_t icu_t96(void) { return icu_called(96); }
static intptr_t icu_t97(void) { return icu_called(97); }
static intptr_t icu_t98(void) { return icu_called(98); }
static intptr_t icu_t99(void) { return icu_called(99); }
static intptr_t icu_t100(void) { return icu_called(100); }
static intptr_t icu_t101(void) { return icu_called(101); }
static intptr_t icu_t102(void) { return icu_called(102); }
static intptr_t icu_t103(void) { return icu_called(103); }
static intptr_t icu_t104(void) { return icu_called(104); }
static intptr_t icu_t105(void) { return icu_called(105); }
static intptr_t icu_t106(void) { return icu_called(106); }
static intptr_t icu_t107(void) { return icu_called(107); }
static intptr_t icu_t108(void) { return icu_called(108); }
static intptr_t icu_t109(void) { return icu_called(109); }
static intptr_t icu_t110(void) { return icu_called(110); }
static intptr_t icu_t111(void) { return icu_called(111); }
static intptr_t icu_t112(void) { return icu_called(112); }
static intptr_t icu_t113(void) { return icu_called(113); }
static intptr_t icu_t114(void) { return icu_called(114); }
static intptr_t icu_t115(void) { return icu_called(115); }
static intptr_t icu_t116(void) { return icu_called(116); }
static intptr_t icu_t117(void) { return icu_called(117); }
static intptr_t icu_t118(void) { return icu_called(118); }
static intptr_t icu_t119(void) { return icu_called(119); }
static intptr_t icu_t120(void) { return icu_called(120); }
static intptr_t icu_t121(void) { return icu_called(121); }
static intptr_t icu_t122(void) { return icu_called(122); }
static intptr_t icu_t123(void) { return icu_called(123); }
static intptr_t icu_t124(void) { return icu_called(124); }
static intptr_t icu_t125(void) { return icu_called(125); }
static intptr_t icu_t126(void) { return icu_called(126); }
static intptr_t icu_t127(void) { return icu_called(127); }
static intptr_t icu_t128(void) { return icu_called(128); }
static intptr_t icu_t129(void) { return icu_called(129); }
static intptr_t icu_t130(void) { return icu_called(130); }
static intptr_t icu_t131(void) { return icu_called(131); }
static intptr_t icu_t132(void) { return icu_called(132); }
static intptr_t icu_t133(void) { return icu_called(133); }
static intptr_t icu_t134(void) { return icu_called(134); }
static intptr_t icu_t135(void) { return icu_called(135); }
static intptr_t icu_t136(void) { return icu_called(136); }
static intptr_t icu_t137(void) { return icu_called(137); }
static intptr_t icu_t138(void) { return icu_called(138); }
static intptr_t icu_t139(void) { return icu_called(139); }
static intptr_t icu_t140(void) { return icu_called(140); }
static intptr_t icu_t141(void) { return icu_called(141); }
static intptr_t icu_t142(void) { return icu_called(142); }
static intptr_t icu_t143(void) { return icu_called(143); }
static intptr_t icu_t144(void) { return icu_called(144); }
static intptr_t icu_t145(void) { return icu_called(145); }
static intptr_t icu_t146(void) { return icu_called(146); }
static intptr_t icu_t147(void) { return icu_called(147); }
static intptr_t icu_t148(void) { return icu_called(148); }
static intptr_t icu_t149(void) { return icu_called(149); }
static intptr_t icu_t150(void) { return icu_called(150); }
static intptr_t icu_t151(void) { return icu_called(151); }
static intptr_t icu_t152(void) { return icu_called(152); }
static intptr_t icu_t153(void) { return icu_called(153); }
static intptr_t icu_t154(void) { return icu_called(154); }
static intptr_t icu_t155(void) { return icu_called(155); }
static intptr_t icu_t156(void) { return icu_called(156); }
static intptr_t icu_t157(void) { return icu_called(157); }
static intptr_t icu_t158(void) { return icu_called(158); }
static intptr_t icu_t159(void) { return icu_called(159); }
static intptr_t icu_t160(void) { return icu_called(160); }
static intptr_t icu_t161(void) { return icu_called(161); }
static intptr_t icu_t162(void) { return icu_called(162); }
static intptr_t icu_t163(void) { return icu_called(163); }
static intptr_t icu_t164(void) { return icu_called(164); }
static intptr_t icu_t165(void) { return icu_called(165); }
static intptr_t icu_t166(void) { return icu_called(166); }
static intptr_t icu_t167(void) { return icu_called(167); }
static intptr_t icu_t168(void) { return icu_called(168); }
static intptr_t icu_t169(void) { return icu_called(169); }
static intptr_t icu_t170(void) { return icu_called(170); }
static intptr_t icu_t171(void) { return icu_called(171); }
static intptr_t icu_t172(void) { return icu_called(172); }
static intptr_t icu_t173(void) { return icu_called(173); }
static intptr_t icu_t174(void) { return icu_called(174); }
static intptr_t icu_t175(void) { return icu_called(175); }
static intptr_t icu_t176(void) { return icu_called(176); }
static intptr_t icu_t177(void) { return icu_called(177); }
static intptr_t icu_t178(void) { return icu_called(178); }
static intptr_t icu_t179(void) { return icu_called(179); }
static intptr_t icu_t180(void) { return icu_called(180); }
static intptr_t icu_t181(void) { return icu_called(181); }
static intptr_t icu_t182(void) { return icu_called(182); }
static intptr_t icu_t183(void) { return icu_called(183); }
static intptr_t icu_t184(void) { return icu_called(184); }
static intptr_t icu_t185(void) { return icu_called(185); }
static intptr_t icu_t186(void) { return icu_called(186); }
static intptr_t icu_t187(void) { return icu_called(187); }
static intptr_t icu_t188(void) { return icu_called(188); }
static intptr_t icu_t189(void) { return icu_called(189); }
static intptr_t icu_t190(void) { return icu_called(190); }
static intptr_t icu_t191(void) { return icu_called(191); }

static intptr_t (*const g_traps[MAX_ICU_SYMBOLS])(void) = {
  icu_t0,
  icu_t1,
  icu_t2,
  icu_t3,
  icu_t4,
  icu_t5,
  icu_t6,
  icu_t7,
  icu_t8,
  icu_t9,
  icu_t10,
  icu_t11,
  icu_t12,
  icu_t13,
  icu_t14,
  icu_t15,
  icu_t16,
  icu_t17,
  icu_t18,
  icu_t19,
  icu_t20,
  icu_t21,
  icu_t22,
  icu_t23,
  icu_t24,
  icu_t25,
  icu_t26,
  icu_t27,
  icu_t28,
  icu_t29,
  icu_t30,
  icu_t31,
  icu_t32,
  icu_t33,
  icu_t34,
  icu_t35,
  icu_t36,
  icu_t37,
  icu_t38,
  icu_t39,
  icu_t40,
  icu_t41,
  icu_t42,
  icu_t43,
  icu_t44,
  icu_t45,
  icu_t46,
  icu_t47,
  icu_t48,
  icu_t49,
  icu_t50,
  icu_t51,
  icu_t52,
  icu_t53,
  icu_t54,
  icu_t55,
  icu_t56,
  icu_t57,
  icu_t58,
  icu_t59,
  icu_t60,
  icu_t61,
  icu_t62,
  icu_t63,
  icu_t64,
  icu_t65,
  icu_t66,
  icu_t67,
  icu_t68,
  icu_t69,
  icu_t70,
  icu_t71,
  icu_t72,
  icu_t73,
  icu_t74,
  icu_t75,
  icu_t76,
  icu_t77,
  icu_t78,
  icu_t79,
  icu_t80,
  icu_t81,
  icu_t82,
  icu_t83,
  icu_t84,
  icu_t85,
  icu_t86,
  icu_t87,
  icu_t88,
  icu_t89,
  icu_t90,
  icu_t91,
  icu_t92,
  icu_t93,
  icu_t94,
  icu_t95,
  icu_t96,
  icu_t97,
  icu_t98,
  icu_t99,
  icu_t100,
  icu_t101,
  icu_t102,
  icu_t103,
  icu_t104,
  icu_t105,
  icu_t106,
  icu_t107,
  icu_t108,
  icu_t109,
  icu_t110,
  icu_t111,
  icu_t112,
  icu_t113,
  icu_t114,
  icu_t115,
  icu_t116,
  icu_t117,
  icu_t118,
  icu_t119,
  icu_t120,
  icu_t121,
  icu_t122,
  icu_t123,
  icu_t124,
  icu_t125,
  icu_t126,
  icu_t127,
  icu_t128,
  icu_t129,
  icu_t130,
  icu_t131,
  icu_t132,
  icu_t133,
  icu_t134,
  icu_t135,
  icu_t136,
  icu_t137,
  icu_t138,
  icu_t139,
  icu_t140,
  icu_t141,
  icu_t142,
  icu_t143,
  icu_t144,
  icu_t145,
  icu_t146,
  icu_t147,
  icu_t148,
  icu_t149,
  icu_t150,
  icu_t151,
  icu_t152,
  icu_t153,
  icu_t154,
  icu_t155,
  icu_t156,
  icu_t157,
  icu_t158,
  icu_t159,
  icu_t160,
  icu_t161,
  icu_t162,
  icu_t163,
  icu_t164,
  icu_t165,
  icu_t166,
  icu_t167,
  icu_t168,
  icu_t169,
  icu_t170,
  icu_t171,
  icu_t172,
  icu_t173,
  icu_t174,
  icu_t175,
  icu_t176,
  icu_t177,
  icu_t178,
  icu_t179,
  icu_t180,
  icu_t181,
  icu_t182,
  icu_t183,
  icu_t184,
  icu_t185,
  icu_t186,
  icu_t187,
  icu_t188,
  icu_t189,
  icu_t190,
  icu_t191
};

/* ------------------------------------------------------------------------ */
/* The few that have to be real                                              */
/* ------------------------------------------------------------------------ */

/* The probe answered its question, and the answer was not "a handful of
 * casing calls". GlobalizationNative_GetDefaultLocaleName runs during game
 * construction and does this:
 *
 *     const char *loc = uloc_getDefault();
 *     if (strcmp(loc, "en_US_POSIX") == 0) loc = "";
 *     uloc_getBaseName(loc, buf, 157, &status);
 *     u_charsToUChars(buf, out, strnlen(buf, 157) + 1);
 *
 * Returning zero from uloc_getDefault put a NULL into strcmp, which is a read
 * of address 0 inside the host's strcmp -- the crash this replaces.
 *
 * Invariant globalization cannot be switched on from outside to avoid this:
 * DOTNET_SYSTEM_GLOBALIZATION_INVARIANT is set and the runtime loaded ICU
 * anyway, because for NativeAOT the switch is baked at compile time. So these
 * three are implemented properly rather than stubbed. They are honest, not
 * approximations: this system has no locale configuration, and the three
 * together produce exactly that -- the root locale. */

static void icu_note(const char *name, int *once) {
  if (*once) return;
  *once = 1;
  debug_log("[icu] CALLED %s (implemented)\n", name);
}

/* What real ICU returns when nothing in the environment sets a locale. The
 * runtime recognises it and substitutes the root locale itself, which is the
 * outcome we want and its own well-trodden path. */
static const char *icu_uloc_getDefault(void) {
  static int once;
  icu_note("uloc_getDefault", &once);
  return "en_US_POSIX";
}

/* The locale id with any @keyword suffix removed. */
static int32_t icu_uloc_getBaseName(const char *localeID, char *name,
                                    int32_t nameCapacity, int32_t *status) {
  static int once;
  icu_note("uloc_getBaseName", &once);
  if (status && *status > 0) return 0;      /* U_FAILURE on entry */
  if (!localeID) localeID = "";

  int32_t n = (int32_t)strcspn(localeID, "@");
  if (name && nameCapacity > 0) {
    int32_t copy = n < nameCapacity - 1 ? n : nameCapacity - 1;
    memcpy(name, localeID, (size_t)copy);
    name[copy] = '\0';
  }
  if (status && n >= nameCapacity) *status = 15;  /* U_BUFFER_OVERFLOW_ERROR */
  return n;
}

/* Widen invariant (ASCII) characters. Not a general conversion, and it does
 * not need to be: ICU defines it only over the invariant character set. */
static void icu_u_charsToUChars(const char *cs, uint16_t *us, int32_t length) {
  static int once;
  icu_note("u_charsToUChars", &once);
  if (!cs || !us) return;
  for (int32_t i = 0; i < length; i++) us[i] = (uint16_t)(unsigned char)cs[i];
}

/* --- collation ---------------------------------------------------------- */
/*
 * Six ucol_* calls showed up in the log, all on trampolines returning zero.
 * That did not crash, and that is exactly the problem: ucol_strcoll returning
 * 0 means "these strings are equal", so with a trampoline EVERY comparison
 * says equal. Sorted lists come out in arbitrary order and lookups by name
 * find the wrong entry, with nothing in the log to suggest why. Silent wrong
 * answers are worse here than a named failure.
 *
 * These implement ordinal (code-unit) ordering, which is what the root locale
 * gets us anyway now that the default locale resolves to invariant. It is not
 * full UCA collation -- no locale-specific tailoring, no case/accent
 * weighting -- but it is a correct, consistent total order, which is what the
 * callers actually depend on.
 */

/* A non-NULL handle. The runtime stores whatever ucol_open returns and hands
 * it back; it is never dereferenced here, but returning NULL invites a caller
 * to treat the collator as missing. */
static int g_collator_sentinel;

static void *icu_ucol_open(const char *loc, int32_t *status) {
  static int once;
  icu_note("ucol_open", &once);
  (void)loc;
  if (status) *status = 0;               /* U_ZERO_ERROR */
  return &g_collator_sentinel;
}

static void *icu_ucol_openRules(const uint16_t *rules, int32_t rulesLength,
                                int32_t normalizationMode, int32_t strength,
                                void *parseError, int32_t *status) {
  static int once;
  icu_note("ucol_openRules", &once);
  (void)rules; (void)rulesLength; (void)normalizationMode; (void)strength;
  (void)parseError;
  if (status) *status = 0;
  return &g_collator_sentinel;
}

static void icu_ucol_close(void *coll) { (void)coll; }

/* UCOL_TERTIARY. */
static int32_t icu_ucol_getStrength(const void *coll) {
  static int once;
  icu_note("ucol_getStrength", &once);
  (void)coll;
  return 2;
}

/* No tailoring, so the rule set is empty. Must still be a valid pointer. */
static const uint16_t *icu_ucol_getRules(const void *coll, int32_t *length) {
  static int once;
  icu_note("ucol_getRules", &once);
  static const uint16_t empty[1] = { 0 };
  (void)coll;
  if (length) *length = 0;
  return empty;
}

static void icu_ucol_setAttribute(void *coll, int32_t attr, int32_t value,
                                  int32_t *status) {
  static int once;
  icu_note("ucol_setAttribute", &once);
  (void)coll; (void)attr; (void)value;
  if (status) *status = 0;               /* accepted, and ignored */
}

/* UCOL_DEFAULT. */
static int32_t icu_ucol_getAttribute(const void *coll, int32_t attr,
                                     int32_t *status) {
  (void)coll; (void)attr;
  if (status) *status = 0;
  return -1;
}

/* Length -1 means NUL-terminated, which ICU callers do use. */
static int32_t u16_len(const uint16_t *s, int32_t len) {
  if (len >= 0) return len;
  int32_t n = 0;
  if (s) while (s[n]) n++;
  return n;
}

static int32_t icu_ucol_strcoll(const void *coll,
                                const uint16_t *a, int32_t alen,
                                const uint16_t *b, int32_t blen) {
  static int once;
  icu_note("ucol_strcoll", &once);
  (void)coll;
  int32_t na = u16_len(a, alen), nb = u16_len(b, blen);
  int32_t n  = na < nb ? na : nb;
  for (int32_t i = 0; i < n; i++)
    if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
  return na == nb ? 0 : (na < nb ? -1 : 1);
}

static int32_t icu_ucol_strcollUTF8(const void *coll,
                                    const char *a, int32_t alen,
                                    const char *b, int32_t blen,
                                    int32_t *status) {
  (void)coll;
  if (status) *status = 0;
  int32_t na = alen >= 0 ? alen : (int32_t)strlen(a ? a : "");
  int32_t nb = blen >= 0 ? blen : (int32_t)strlen(b ? b : "");
  int32_t n  = na < nb ? na : nb;
  for (int32_t i = 0; i < n; i++) {
    unsigned char ca = (unsigned char)a[i], cb = (unsigned char)b[i];
    if (ca != cb) return ca < cb ? -1 : 1;
  }
  return na == nb ? 0 : (na < nb ? -1 : 1);
}

/* ICU sort keys are NUL-terminated byte strings, compared with strcmp
 * semantics, so no byte of the key may be zero. Big-endian code units fail
 * that immediately: every ASCII character has a 0x00 high byte, and a
 * comparison would stop at the first one. So each code unit goes out as three
 * base-255 digits, each offset by 1 into 0x01..0xFF. That is order-preserving
 * -- lexicographic byte order equals numeric code-unit order -- and never
 * emits a zero, so a key comparison agrees with ucol_strcoll. A sort key that
 * disagreed with strcoll would be worse than none at all. */
static int32_t icu_ucol_getSortKey(const void *coll, const uint16_t *src,
                                   int32_t srcLength, uint8_t *result,
                                   int32_t resultLength) {
  static int once;
  icu_note("ucol_getSortKey", &once);
  (void)coll;
  int32_t n = u16_len(src, srcLength);
  int32_t need = n * 3 + 1;
  if (result && resultLength >= need) {
    for (int32_t i = 0; i < n; i++) {
      unsigned c = src[i];
      result[i * 3]     = (uint8_t)(c / (255u * 255u) + 1u);
      result[i * 3 + 1] = (uint8_t)((c / 255u) % 255u + 1u);
      result[i * 3 + 2] = (uint8_t)(c % 255u + 1u);
    }
    result[n * 3] = 0;
  }
  return need;                            /* required size, per ICU */
}

/* --- case mapping and character classification ---------------------------
 *
 * This is the bug the whole reanim hunt was chasing.
 *
 * `u_tolower_68` and `u_toupper_68` were on trampolines, and a trampoline
 * returns 0. u_tolower(c) returning 0 means every character maps to NUL, so a
 * name that is case-normalised before lookup becomes an empty string. That is
 * precisely the error the game reports:
 *
 *     Failed to load reanim from RSB: <nothing>
 *
 * The log has `[icu] CALLED u_toupper_68` immediately before the failure, and
 * `u_tolower_68` immediately before the resource bundle is opened. Silent
 * wrong answers again -- the trampolines never crashed, they just quietly
 * turned every string into nothing.
 *
 * ASCII is handled exactly. Above that, characters are returned unchanged
 * rather than guessed at: full Unicode case mapping needs the tables we do not
 * have, and returning the character unchanged is correct for every ASCII
 * identifier the game actually looks up, whereas returning 0 is correct for
 * nothing at all.
 */
typedef int32_t UChar32;

static UChar32 icu_u_tolower(UChar32 c) {
  static int once;
  icu_note("u_tolower", &once);
  return (c >= 'A' && c <= 'Z') ? c + 32 : c;
}

static UChar32 icu_u_toupper(UChar32 c) {
  static int once;
  icu_note("u_toupper", &once);
  return (c >= 'a' && c <= 'z') ? c - 32 : c;
}

static UChar32 icu_u_totitle(UChar32 c) { return icu_u_toupper(c); }

static UChar32 icu_u_foldCase(UChar32 c, uint32_t options) {
  (void)options;
  return icu_u_tolower(c);
}

/* Classification. ASCII exactly; anything above it is reported as a letter,
 * which is the least destructive answer for identifier text. */
static int8_t icu_u_isalpha(UChar32 c) {
  return (int8_t)((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c > 0x7f);
}
static int8_t icu_u_isdigit(UChar32 c)  { return (int8_t)(c >= '0' && c <= '9'); }
static int8_t icu_u_isalnum(UChar32 c)  { return (int8_t)(icu_u_isalpha(c) || icu_u_isdigit(c)); }
static int8_t icu_u_isspace(UChar32 c)  { return (int8_t)(c == ' ' || (c >= 9 && c <= 13)); }
static int8_t icu_u_isblank(UChar32 c)  { return (int8_t)(c == ' ' || c == '\t'); }
static int8_t icu_u_isupper(UChar32 c)  { return (int8_t)(c >= 'A' && c <= 'Z'); }
static int8_t icu_u_islower(UChar32 c)  { return (int8_t)(c >= 'a' && c <= 'z'); }
static int8_t icu_u_iscntrl(UChar32 c)  { return (int8_t)(c < 0x20 || c == 0x7f); }
static int8_t icu_u_isprint(UChar32 c)  { return (int8_t)(c >= 0x20 && c != 0x7f); }
static int8_t icu_u_ispunct(UChar32 c)  {
  return (int8_t)(c < 0x80 && c > 0x20 && !icu_u_isalnum(c) && c != 0x7f);
}
static int8_t icu_u_isxdigit(UChar32 c) {
  return (int8_t)(icu_u_isdigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'));
}

/* U_UPPERCASE_LETTER 1, U_LOWERCASE_LETTER 2, U_DECIMAL_DIGIT_NUMBER 9,
 * U_SPACE_SEPARATOR 12, U_OTHER_PUNCTUATION 23, U_CONTROL 15. */
static int8_t icu_u_charType(UChar32 c) {
  if (icu_u_isupper(c)) return 1;
  if (icu_u_islower(c)) return 2;
  if (c > 0x7f)         return 2;
  if (icu_u_isdigit(c)) return 9;
  if (c == ' ')         return 12;
  if (icu_u_iscntrl(c)) return 15;
  return 23;
}

/* Whole-string case mapping.
 * (dest, destCapacity, src, srcLength, locale, status) -- srcLength -1 means
 * NUL-terminated. Returns the length needed, per ICU. */
static int32_t str_case(uint16_t *dest, int32_t cap, const uint16_t *src,
                        int32_t srcLen, int32_t *status, int up) {
  int32_t n = u16_len(src, srcLen);
  if (dest && cap >= n) {
    for (int32_t i = 0; i < n; i++)
      dest[i] = (uint16_t)(up ? icu_u_toupper(src[i]) : icu_u_tolower(src[i]));
    if (cap > n) dest[n] = 0;
  } else if (status) {
    *status = 15;                 /* U_BUFFER_OVERFLOW_ERROR */
  }
  return n;
}

static int32_t icu_u_strToLower(uint16_t *d, int32_t cap, const uint16_t *s,
                                int32_t sl, const char *loc, int32_t *st) {
  static int once;
  icu_note("u_strToLower", &once);
  (void)loc;
  return str_case(d, cap, s, sl, st, 0);
}

static int32_t icu_u_strToUpper(uint16_t *d, int32_t cap, const uint16_t *s,
                                int32_t sl, const char *loc, int32_t *st) {
  static int once;
  icu_note("u_strToUpper", &once);
  (void)loc;
  return str_case(d, cap, s, sl, st, 1);
}

static int32_t icu_u_strFoldCase(uint16_t *d, int32_t cap, const uint16_t *s,
                                 int32_t sl, uint32_t opt, int32_t *st) {
  (void)opt;
  return str_case(d, cap, s, sl, st, 0);
}

/* --- string conversion, search and break iteration -----------------------
 *
 * These four were still on trampolines and are called in the order
 *
 *     u_uastrncpy -> ubrk_openRules -> usearch_openFromCollator -> usearch_first
 *
 * immediately before the RSB reports `DecompressionTask ... gpuDecompressed=0`.
 * By the rule the last round established, a trampoline does not crash -- it
 * returns a silent wrong answer:
 *
 *   u_uastrncpy returns 0 AND NEVER WRITES ITS DESTINATION, so anything built
 *     through it reads uninitialised memory.
 *   usearch_first returns 0, which means "match at index 0" -- so a
 *     culture-aware IndexOf that should say "not found" says "found at the
 *     start" instead. .NET's CompareInfo.IndexOf/IsPrefix/IsSuffix are built on
 *     usearch, so that is every culture-sensitive string search in the runtime
 *     answering wrongly rather than failing.
 *
 * Search is ordinal, matching the collation implemented above, so the two agree
 * with each other. Break iteration treats every code unit as a boundary, which
 * is what a character break iterator does and is correct for the ASCII
 * identifiers actually in play.
 */

/* -- conversion between invariant chars and UChars -- */

static uint16_t *icu_u_uastrncpy(uint16_t *dst, const char *src, int32_t n) {
  static int once;
  icu_note("u_uastrncpy", &once);
  if (!dst || !src) return dst;
  int32_t i = 0;
  for (; i < n && src[i]; i++) dst[i] = (uint16_t)(unsigned char)src[i];
  if (i < n) dst[i] = 0;               /* NUL-terminate when there is room */
  return dst;
}

static uint16_t *icu_u_uastrcpy(uint16_t *dst, const char *src) {
  if (!dst || !src) return dst;
  int32_t i = 0;
  for (; src[i]; i++) dst[i] = (uint16_t)(unsigned char)src[i];
  dst[i] = 0;
  return dst;
}

static char *icu_u_austrncpy(char *dst, const uint16_t *src, int32_t n) {
  if (!dst || !src) return dst;
  int32_t i = 0;
  for (; i < n && src[i]; i++) dst[i] = (char)(src[i] < 0x80 ? src[i] : '?');
  if (i < n) dst[i] = 0;
  return dst;
}

static char *icu_u_austrcpy(char *dst, const uint16_t *src) {
  if (!dst || !src) return dst;
  int32_t i = 0;
  for (; src[i]; i++) dst[i] = (char)(src[i] < 0x80 ? src[i] : '?');
  dst[i] = 0;
  return dst;
}

/* -- string search -- */

#define USEARCH_DONE (-1)

typedef struct {
  int             in_use;
  const uint16_t *pat; int32_t patLen;
  const uint16_t *txt; int32_t txtLen;
  int32_t         offset;      /* where the next search starts */
  int32_t         matchLen;
} Usearch;

static Usearch g_usearch[8];

static int32_t find_at(const Usearch *u, int32_t from) {
  /* An empty pattern is found immediately -- IndexOf("") is 0, not -1. Caught
   * by the host test, which is why the test was written before shipping. */
  if (u->patLen == 0) return (from <= u->txtLen) ? from : USEARCH_DONE;
  if (u->patLen < 0 || u->txtLen < u->patLen) return USEARCH_DONE;
  for (int32_t i = from; i + u->patLen <= u->txtLen; i++) {
    int32_t k = 0;
    while (k < u->patLen && u->txt[i + k] == u->pat[k]) k++;
    if (k == u->patLen) return i;
  }
  return USEARCH_DONE;
}

static void *icu_usearch_openFromCollator(const uint16_t *pat, int32_t patLen,
                                          const uint16_t *txt, int32_t txtLen,
                                          const void *coll, void *bi,
                                          int32_t *status) {
  static int once;
  icu_note("usearch_openFromCollator", &once);
  (void)coll; (void)bi;
  for (size_t i = 0; i < sizeof(g_usearch)/sizeof(g_usearch[0]); i++) {
    if (g_usearch[i].in_use) continue;
    Usearch *u = &g_usearch[i];
    u->in_use = 1;
    u->pat = pat; u->patLen = u16_len(pat, patLen);
    u->txt = txt; u->txtLen = u16_len(txt, txtLen);
    u->offset = 0; u->matchLen = 0;
    if (status) *status = 0;
    return u;
  }
  if (status) *status = 7;             /* U_MEMORY_ALLOCATION_ERROR */
  return NULL;
}

static int32_t icu_usearch_first(void *h, int32_t *status) {
  static int once;
  icu_note("usearch_first", &once);
  Usearch *u = (Usearch *)h;
  if (status) *status = 0;
  if (!u) return USEARCH_DONE;
  int32_t at = find_at(u, 0);
  u->matchLen = (at >= 0) ? u->patLen : 0;
  u->offset   = (at >= 0) ? at + 1 : u->txtLen;
  return at;
}

static int32_t icu_usearch_next(void *h, int32_t *status) {
  Usearch *u = (Usearch *)h;
  if (status) *status = 0;
  if (!u) return USEARCH_DONE;
  int32_t at = find_at(u, u->offset);
  u->matchLen = (at >= 0) ? u->patLen : 0;
  u->offset   = (at >= 0) ? at + 1 : u->txtLen;
  return at;
}

static int32_t icu_usearch_last(void *h, int32_t *status) {
  Usearch *u = (Usearch *)h;
  if (status) *status = 0;
  if (!u) return USEARCH_DONE;
  int32_t best = USEARCH_DONE, at = find_at(u, 0);
  while (at >= 0) { best = at; at = find_at(u, at + 1); }
  u->matchLen = (best >= 0) ? u->patLen : 0;
  return best;
}

static int32_t icu_usearch_previous(void *h, int32_t *status) {
  return icu_usearch_last(h, status);
}

static int32_t icu_usearch_getMatchedLength(const void *h) {
  const Usearch *u = (const Usearch *)h;
  return u ? u->matchLen : 0;
}

static int32_t icu_usearch_getMatchedStart(const void *h) {
  const Usearch *u = (const Usearch *)h;
  return (u && u->matchLen) ? u->offset - 1 : USEARCH_DONE;
}

static void icu_usearch_close(void *h) {
  Usearch *u = (Usearch *)h;
  if (u) u->in_use = 0;
}

static void icu_usearch_reset(void *h) {
  Usearch *u = (Usearch *)h;
  if (u) { u->offset = 0; u->matchLen = 0; }
}

/* Reusing one searcher for a different string is the normal pattern, and
 * these are how it is done. Left as trampolines they do nothing at all, so the
 * searcher keeps its ORIGINAL pattern and text and cheerfully answers
 * questions about the wrong strings -- a wrong answer, not a failure, which is
 * the shape that has cost this project the most time.
 *
 * They only surfaced now because implementing usearch_openFromCollator let the
 * game get far enough to call them. Worth remembering: implementing one ICU
 * function reveals the next, so the list is not finished until a run shows
 * every [icu] CALLED line saying (implemented). */
static void icu_usearch_setText(void *h, const uint16_t *txt, int32_t len,
                                int32_t *status) {
  static int once;
  icu_note("usearch_setText", &once);
  Usearch *u = (Usearch *)h;
  if (status) *status = 0;
  if (!u) return;
  u->txt = txt;
  u->txtLen = u16_len(txt, len);
  u->offset = 0;
  u->matchLen = 0;
}

static void icu_usearch_setPattern(void *h, const uint16_t *pat, int32_t len,
                                   int32_t *status) {
  static int once;
  icu_note("usearch_setPattern", &once);
  Usearch *u = (Usearch *)h;
  if (status) *status = 0;
  if (!u) return;
  u->pat = pat;
  u->patLen = u16_len(pat, len);
  u->offset = 0;
  u->matchLen = 0;
}

static const uint16_t *icu_usearch_getText(const void *h, int32_t *len) {
  const Usearch *u = (const Usearch *)h;
  if (len) *len = u ? u->txtLen : 0;
  return u ? u->txt : NULL;
}

static const uint16_t *icu_usearch_getPattern(const void *h, int32_t *len) {
  const Usearch *u = (const Usearch *)h;
  if (len) *len = u ? u->patLen : 0;
  return u ? u->pat : NULL;
}

static void icu_usearch_setCollator(void *h, const void *coll, int32_t *status) {
  (void)h; (void)coll;
  if (status) *status = 0;
}

static void icu_usearch_setOffset(void *h, int32_t off, int32_t *status) {
  Usearch *u = (Usearch *)h;
  if (status) *status = 0;
  if (u) u->offset = off;
}

/* -- break iteration -- */

#define UBRK_DONE (-1)

typedef struct {
  int             in_use;
  const uint16_t *txt; int32_t txtLen;
  int32_t         pos;
} Ubrk;

static Ubrk g_ubrk[8];

static void *ubrk_alloc(const uint16_t *txt, int32_t len, int32_t *status) {
  for (size_t i = 0; i < sizeof(g_ubrk)/sizeof(g_ubrk[0]); i++) {
    if (g_ubrk[i].in_use) continue;
    Ubrk *b = &g_ubrk[i];
    b->in_use = 1; b->txt = txt; b->txtLen = u16_len(txt, len); b->pos = 0;
    if (status) *status = 0;
    return b;
  }
  if (status) *status = 7;
  return NULL;
}

static void *icu_ubrk_openRules(const uint16_t *rules, int32_t rulesLen,
                                const uint16_t *txt, int32_t txtLen,
                                void *parseErr, int32_t *status) {
  static int once;
  icu_note("ubrk_openRules", &once);
  (void)rules; (void)rulesLen; (void)parseErr;
  return ubrk_alloc(txt, txtLen, status);
}

static void *icu_ubrk_open(int32_t type, const char *loc,
                           const uint16_t *txt, int32_t txtLen,
                           int32_t *status) {
  (void)type; (void)loc;
  return ubrk_alloc(txt, txtLen, status);
}

static void icu_ubrk_close(void *h) { Ubrk *b = h; if (b) b->in_use = 0; }

static void icu_ubrk_setText(void *h, const uint16_t *txt, int32_t len,
                             int32_t *status) {
  Ubrk *b = h;
  if (status) *status = 0;
  if (b) { b->txt = txt; b->txtLen = u16_len(txt, len); b->pos = 0; }
}

static int32_t icu_ubrk_first(void *h)   { Ubrk *b = h; if (!b) return UBRK_DONE; b->pos = 0; return 0; }
static int32_t icu_ubrk_last(void *h)    { Ubrk *b = h; if (!b) return UBRK_DONE; b->pos = b->txtLen; return b->txtLen; }
static int32_t icu_ubrk_current(void *h) { Ubrk *b = h; return b ? b->pos : UBRK_DONE; }

static int32_t icu_ubrk_next(void *h) {
  Ubrk *b = h;
  if (!b || b->pos >= b->txtLen) return UBRK_DONE;
  return ++b->pos;
}

static int32_t icu_ubrk_previous(void *h) {
  Ubrk *b = h;
  if (!b || b->pos <= 0) return UBRK_DONE;
  return --b->pos;
}

static int32_t icu_ubrk_following(void *h, int32_t off) {
  Ubrk *b = h;
  if (!b || off + 1 > b->txtLen) return UBRK_DONE;
  b->pos = off + 1;
  return b->pos;
}

static int32_t icu_ubrk_preceding(void *h, int32_t off) {
  Ubrk *b = h;
  if (!b || off <= 0) return UBRK_DONE;
  b->pos = off - 1;
  return b->pos;
}

static int8_t icu_ubrk_isBoundary(void *h, int32_t off) {
  Ubrk *b = h;
  return (int8_t)(b && off >= 0 && off <= b->txtLen);
}

/* The last three trampolines.
 *
 * Two are harmless -- they return a length, and zero means "not found", which
 * is a legitimate answer. The third is not: ulocdata_getCLDRVersion WRITES its
 * output and a trampoline does not, leaving the caller's version array
 * uninitialised. That is the same class of bug as u_uastrncpy, which is worth
 * closing on principle rather than waiting to be bitten by it a third time. */
static void icu_ulocdata_getCLDRVersion(uint8_t *versionArray, int32_t *status) {
  static int once;
  icu_note("ulocdata_getCLDRVersion", &once);
  if (versionArray) { versionArray[0] = 46; versionArray[1] = 0;
                      versionArray[2] = 0;  versionArray[3] = 0; }
  if (status) *status = 0;
}

/* No keywords on the root locale, so an empty result of length 0. The buffer
 * is NUL-terminated rather than left untouched. */
static int32_t icu_uloc_getKeywordValue(const char *locale, const char *keyword,
                                        char *buffer, int32_t capacity,
                                        int32_t *status) {
  static int once;
  icu_note("uloc_getKeywordValue", &once);
  (void)locale; (void)keyword;
  if (buffer && capacity > 0) buffer[0] = 0;
  if (status) *status = 0;
  return 0;
}

/* No Windows timezone mapping without the CLDR data. Zero length, empty
 * buffer, and the caller falls back -- which is what it is written to do. */
static int32_t icu_ucal_getWindowsTimeZoneID(const uint16_t *id, int32_t len,
                                             uint16_t *winid, int32_t cap,
                                             int32_t *status) {
  static int once;
  icu_note("ucal_getWindowsTimeZoneID", &once);
  (void)id; (void)len;
  if (winid && cap > 0) winid[0] = 0;
  if (status) *status = 0;
  return 0;
}

typedef struct { const char *name; void *fn; } IcuReal;

static const IcuReal g_icu_real[] = {
  { "u_tolower",      (void *)icu_u_tolower },
  { "u_toupper",      (void *)icu_u_toupper },
  { "u_totitle",      (void *)icu_u_totitle },
  { "u_foldCase",     (void *)icu_u_foldCase },
  { "u_isalpha",      (void *)icu_u_isalpha },
  { "u_isdigit",      (void *)icu_u_isdigit },
  { "u_isalnum",      (void *)icu_u_isalnum },
  { "u_isspace",      (void *)icu_u_isspace },
  { "u_isblank",      (void *)icu_u_isblank },
  { "u_isupper",      (void *)icu_u_isupper },
  { "u_islower",      (void *)icu_u_islower },
  { "u_iscntrl",      (void *)icu_u_iscntrl },
  { "u_isprint",      (void *)icu_u_isprint },
  { "u_ispunct",      (void *)icu_u_ispunct },
  { "u_isxdigit",     (void *)icu_u_isxdigit },
  { "u_charType",     (void *)icu_u_charType },
  { "u_strToLower",   (void *)icu_u_strToLower },
  { "u_strToUpper",   (void *)icu_u_strToUpper },
  { "u_strFoldCase",  (void *)icu_u_strFoldCase },
  { "u_uastrncpy",  (void *)icu_u_uastrncpy },
  { "u_uastrcpy",   (void *)icu_u_uastrcpy },
  { "u_austrncpy",  (void *)icu_u_austrncpy },
  { "u_austrcpy",   (void *)icu_u_austrcpy },
  { "usearch_openFromCollator", (void *)icu_usearch_openFromCollator },
  { "usearch_first",            (void *)icu_usearch_first },
  { "usearch_next",             (void *)icu_usearch_next },
  { "usearch_last",             (void *)icu_usearch_last },
  { "usearch_previous",         (void *)icu_usearch_previous },
  { "usearch_getMatchedLength", (void *)icu_usearch_getMatchedLength },
  { "usearch_getMatchedStart",  (void *)icu_usearch_getMatchedStart },
  { "usearch_close",            (void *)icu_usearch_close },
  { "usearch_reset",            (void *)icu_usearch_reset },
  { "usearch_setText",     (void *)icu_usearch_setText },
  { "usearch_setPattern",  (void *)icu_usearch_setPattern },
  { "usearch_getText",     (void *)icu_usearch_getText },
  { "usearch_getPattern",  (void *)icu_usearch_getPattern },
  { "usearch_setCollator", (void *)icu_usearch_setCollator },
  { "usearch_setOffset",        (void *)icu_usearch_setOffset },
  { "ubrk_openRules",  (void *)icu_ubrk_openRules },
  { "ubrk_open",       (void *)icu_ubrk_open },
  { "ubrk_close",      (void *)icu_ubrk_close },
  { "ubrk_setText",    (void *)icu_ubrk_setText },
  { "ubrk_first",      (void *)icu_ubrk_first },
  { "ubrk_last",       (void *)icu_ubrk_last },
  { "ubrk_current",    (void *)icu_ubrk_current },
  { "ubrk_next",       (void *)icu_ubrk_next },
  { "ubrk_previous",   (void *)icu_ubrk_previous },
  { "ubrk_following",  (void *)icu_ubrk_following },
  { "ubrk_preceding",  (void *)icu_ubrk_preceding },
  { "ubrk_isBoundary", (void *)icu_ubrk_isBoundary },
  { "ulocdata_getCLDRVersion",  (void *)icu_ulocdata_getCLDRVersion },
  { "uloc_getKeywordValue",     (void *)icu_uloc_getKeywordValue },
  { "ucal_getWindowsTimeZoneID",(void *)icu_ucal_getWindowsTimeZoneID },
  { "uloc_getDefault",   (void *)icu_uloc_getDefault },
  { "uloc_getBaseName",  (void *)icu_uloc_getBaseName },
  { "u_charsToUChars",   (void *)icu_u_charsToUChars },
  { "ucol_open",         (void *)icu_ucol_open },
  { "ucol_openRules",    (void *)icu_ucol_openRules },
  { "ucol_close",        (void *)icu_ucol_close },
  { "ucol_getStrength",  (void *)icu_ucol_getStrength },
  { "ucol_getRules",     (void *)icu_ucol_getRules },
  { "ucol_setAttribute", (void *)icu_ucol_setAttribute },
  { "ucol_getAttribute", (void *)icu_ucol_getAttribute },
  { "ucol_strcoll",      (void *)icu_ucol_strcoll },
  { "ucol_strcollUTF8",  (void *)icu_ucol_strcollUTF8 },
  { "ucol_getSortKey",   (void *)icu_ucol_getSortKey },
};

void *icu_probe_symbol(const char *symbol) {
  if (!symbol) return NULL;

  /* Version detection: claim one version, stay silent about the other hundred
   * probes so they do not bury the rest of the log. */
  if (!strncmp(symbol, "u_strlen", 8)) {
    char want[32];
    snprintf(want, sizeof(want), "u_strlen_%d", PRETEND_ICU_VERSION);
    if (strcmp(symbol, want) != 0) return NULL;
    debug_log("[icu] version probe: claiming ICU %d\n", PRETEND_ICU_VERSION);
  }

  /* The runtime asks for versioned names (uloc_getDefault_68). Strip the
   * suffix before matching the implemented set. */
  char base[64], suffix[16];
  snprintf(base, sizeof(base), "%s", symbol);
  snprintf(suffix, sizeof(suffix), "_%d", PRETEND_ICU_VERSION);
  size_t blen = strlen(base), slen = strlen(suffix);
  if (blen > slen && !strcmp(base + blen - slen, suffix)) base[blen - slen] = '\0';

  for (size_t i = 0; i < sizeof(g_icu_real)/sizeof(g_icu_real[0]); i++)
    if (!strcmp(base, g_icu_real[i].name)) return g_icu_real[i].fn;

  /* Same symbol asked for twice: hand back the same trampoline. */
  for (int i = 0; i < g_count; i++)
    if (!strcmp(g_names[i], symbol)) return (void *)g_traps[i];

  if (g_count >= MAX_ICU_SYMBOLS) {
    debug_log("[icu] out of trampolines at %s -- raise MAX_ICU_SYMBOLS\n", symbol);
    return NULL;
  }

  int idx = g_count++;
  snprintf(g_names[idx], sizeof(g_names[idx]), "%s", symbol);
  return (void *)g_traps[idx];
}

int icu_probe_count(void) { return g_count; }

void icu_probe_report(void) {
  int called = 0;
  for (int i = 0; i < g_count; i++) if (g_reported[i]) called++;
  debug_log("[icu] %d symbols resolved, %d distinct actually called "
            "(%d calls total)\n", g_count, called, g_calls);
}
