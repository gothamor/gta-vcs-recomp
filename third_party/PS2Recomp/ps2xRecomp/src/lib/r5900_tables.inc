// [gta-recomp] The CPU and R5900 instruction tables of the recompiler's disassembler (r5900_disasm.cpp):
// generated once from rabbitizer 1.14.3's tables (tables/ and include/generated/), so the decoding and the text
// are the ones the recompiler had from rabbitizer - no longer a dependency. Do not edit by hand; checked word for
// word against rabbitizer before it was removed (docs/PS2RECOMP_PATCHES.md). Rabbitizer's licence:
//
// MIT License
//
// Copyright (c) 2022 Decompollaborate
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

// operand types
enum class Operand : uint8_t
{
    None,
    cpu_rs,
    cpu_rt,
    cpu_immediate,
    cpu_label,
    cpu_branch_target_label,
    cpu_immediate_base,
    cpu_hint,
    cpu_op,
    cpu_ft,
    cpu_cop2t,
    cpu_rd,
    cpu_sa,
    cpu_maybe_rd_rs,
    cpu_code_lower,
    cpu_code,
    cpu_cop0d,
    cpu_fs,
    cpu_cop1cs,
    cpu_fd,
    cpu_cop2cd,
    r5900_vft,
    cpu_zero,
    cpu_copraw,
    r5900_vfs,
    r5900_vis,
    r5900_vfdxyzw,
    r5900_vfsxyzw,
    r5900_vftn,
    r5900_Q,
    r5900_I,
    r5900_vftxyzw,
    r5900_vid,
    r5900_vit,
    r5900_immediate5,
    r5900_immediate15,
    r5900_ACCxyzw,
    r5900_vis_postincr,
    r5900_vit_postincr,
    r5900_vis_predecr,
    r5900_vit_predecr,
    r5900_vfsl,
    r5900_vftm,
    r5900_R,
    r5900_vis_parenthesis,
};

enum Flag : uint32_t
{
    F_isBranch = 1u << 0,
    F_isJump = 1u << 1,
    F_isTrap = 1u << 2,
    F_doesLoad = 1u << 3,
    F_doesStore = 1u << 4,
    F_modifiesRs = 1u << 5,
    F_modifiesRt = 1u << 6,
    F_modifiesRd = 1u << 7,
    F_doesLink = 1u << 8,
    F_modifiesFs = 1u << 9,
    F_modifiesFt = 1u << 10,
    F_modifiesFd = 1u << 11,
    F_modifiesHI = 1u << 12,
    F_modifiesLO = 1u << 13,
    F_isUnsigned = 1u << 14,
};

struct InstrDescriptor
{
    const char *name;
    Operand operands[5];
    bool xyzwSuffix;
    bool valid; // not an INVALID entry
    uint32_t flags;
};

constexpr uint16_t kNoId = 0xFFFF; // a case the table has not: the id stays what it was
constexpr uint16_t ID_cpu_INVALID = 0;
constexpr uint16_t ID_cpu_j = 1;
constexpr uint16_t ID_cpu_jal = 2;
constexpr uint16_t ID_cpu_beq = 3;
constexpr uint16_t ID_cpu_bne = 4;
constexpr uint16_t ID_cpu_beql = 5;
constexpr uint16_t ID_cpu_bnel = 6;
constexpr uint16_t ID_cpu_blez = 7;
constexpr uint16_t ID_cpu_blezl = 8;
constexpr uint16_t ID_cpu_bgtz = 9;
constexpr uint16_t ID_cpu_bgtzl = 10;
constexpr uint16_t ID_cpu_addi = 11;
constexpr uint16_t ID_cpu_addiu = 12;
constexpr uint16_t ID_cpu_slti = 13;
constexpr uint16_t ID_cpu_sltiu = 14;
constexpr uint16_t ID_cpu_andi = 15;
constexpr uint16_t ID_cpu_ori = 16;
constexpr uint16_t ID_cpu_xori = 17;
constexpr uint16_t ID_cpu_daddi = 18;
constexpr uint16_t ID_cpu_daddiu = 19;
constexpr uint16_t ID_cpu_lui = 20;
constexpr uint16_t ID_cpu_ldl = 21;
constexpr uint16_t ID_cpu_ldr = 22;
constexpr uint16_t ID_cpu_lb = 23;
constexpr uint16_t ID_cpu_lh = 24;
constexpr uint16_t ID_cpu_lwl = 25;
constexpr uint16_t ID_cpu_lw = 26;
constexpr uint16_t ID_cpu_lbu = 27;
constexpr uint16_t ID_cpu_lhu = 28;
constexpr uint16_t ID_cpu_lwr = 29;
constexpr uint16_t ID_cpu_lwu = 30;
constexpr uint16_t ID_cpu_sb = 31;
constexpr uint16_t ID_cpu_sh = 32;
constexpr uint16_t ID_cpu_swl = 33;
constexpr uint16_t ID_cpu_sw = 34;
constexpr uint16_t ID_cpu_sdl = 35;
constexpr uint16_t ID_cpu_sdr = 36;
constexpr uint16_t ID_cpu_swr = 37;
constexpr uint16_t ID_cpu_ll = 38;
constexpr uint16_t ID_cpu_pref = 39;
constexpr uint16_t ID_cpu_lld = 40;
constexpr uint16_t ID_cpu_ld = 41;
constexpr uint16_t ID_cpu_sc = 42;
constexpr uint16_t ID_cpu_scd = 43;
constexpr uint16_t ID_cpu_sd = 44;
constexpr uint16_t ID_cpu_cache = 45;
constexpr uint16_t ID_cpu_lwc1 = 46;
constexpr uint16_t ID_cpu_ldc1 = 47;
constexpr uint16_t ID_cpu_swc1 = 48;
constexpr uint16_t ID_cpu_sdc1 = 49;
constexpr uint16_t ID_cpu_lwc2 = 50;
constexpr uint16_t ID_cpu_ldc2 = 51;
constexpr uint16_t ID_cpu_swc2 = 52;
constexpr uint16_t ID_cpu_sdc2 = 53;
constexpr uint16_t ID_cpu_b = 54;
constexpr uint16_t ID_cpu_beqz = 55;
constexpr uint16_t ID_cpu_bnez = 56;
constexpr uint16_t ID_cpu_sll = 57;
constexpr uint16_t ID_cpu_srl = 58;
constexpr uint16_t ID_cpu_sra = 59;
constexpr uint16_t ID_cpu_dsll = 60;
constexpr uint16_t ID_cpu_dsrl = 61;
constexpr uint16_t ID_cpu_dsra = 62;
constexpr uint16_t ID_cpu_dsll32 = 63;
constexpr uint16_t ID_cpu_dsrl32 = 64;
constexpr uint16_t ID_cpu_dsra32 = 65;
constexpr uint16_t ID_cpu_dsllv = 66;
constexpr uint16_t ID_cpu_dsrlv = 67;
constexpr uint16_t ID_cpu_dsrav = 68;
constexpr uint16_t ID_cpu_sllv = 69;
constexpr uint16_t ID_cpu_srlv = 70;
constexpr uint16_t ID_cpu_srav = 71;
constexpr uint16_t ID_cpu_mthi = 72;
constexpr uint16_t ID_cpu_mtlo = 73;
constexpr uint16_t ID_cpu_jr = 74;
constexpr uint16_t ID_cpu_jalr = 75;
constexpr uint16_t ID_cpu_mfhi = 76;
constexpr uint16_t ID_cpu_mflo = 77;
constexpr uint16_t ID_cpu_movz = 78;
constexpr uint16_t ID_cpu_movn = 79;
constexpr uint16_t ID_cpu_div = 80;
constexpr uint16_t ID_cpu_divu = 81;
constexpr uint16_t ID_cpu_sn64_div = 82;
constexpr uint16_t ID_cpu_sn64_divu = 83;
constexpr uint16_t ID_cpu_ddiv = 84;
constexpr uint16_t ID_cpu_ddivu = 85;
constexpr uint16_t ID_cpu_add = 86;
constexpr uint16_t ID_cpu_addu = 87;
constexpr uint16_t ID_cpu_sub = 88;
constexpr uint16_t ID_cpu_subu = 89;
constexpr uint16_t ID_cpu_and = 90;
constexpr uint16_t ID_cpu_or = 91;
constexpr uint16_t ID_cpu_xor = 92;
constexpr uint16_t ID_cpu_nor = 93;
constexpr uint16_t ID_cpu_slt = 94;
constexpr uint16_t ID_cpu_sltu = 95;
constexpr uint16_t ID_cpu_dadd = 96;
constexpr uint16_t ID_cpu_daddu = 97;
constexpr uint16_t ID_cpu_dsub = 98;
constexpr uint16_t ID_cpu_dsubu = 99;
constexpr uint16_t ID_cpu_syscall = 100;
constexpr uint16_t ID_cpu_break = 101;
constexpr uint16_t ID_cpu_sync = 102;
constexpr uint16_t ID_cpu_mult = 103;
constexpr uint16_t ID_cpu_multu = 104;
constexpr uint16_t ID_cpu_dmult = 105;
constexpr uint16_t ID_cpu_dmultu = 106;
constexpr uint16_t ID_cpu_tge = 107;
constexpr uint16_t ID_cpu_tgeu = 108;
constexpr uint16_t ID_cpu_tlt = 109;
constexpr uint16_t ID_cpu_tltu = 110;
constexpr uint16_t ID_cpu_teq = 111;
constexpr uint16_t ID_cpu_tne = 112;
constexpr uint16_t ID_cpu_nop = 113;
constexpr uint16_t ID_cpu_move = 114;
constexpr uint16_t ID_cpu_not = 115;
constexpr uint16_t ID_cpu_neg = 116;
constexpr uint16_t ID_cpu_negu = 117;
constexpr uint16_t ID_cpu_bltz = 118;
constexpr uint16_t ID_cpu_bgez = 119;
constexpr uint16_t ID_cpu_bltzl = 120;
constexpr uint16_t ID_cpu_bgezl = 121;
constexpr uint16_t ID_cpu_tgei = 122;
constexpr uint16_t ID_cpu_tgeiu = 123;
constexpr uint16_t ID_cpu_tlti = 124;
constexpr uint16_t ID_cpu_tltiu = 125;
constexpr uint16_t ID_cpu_teqi = 126;
constexpr uint16_t ID_cpu_tnei = 127;
constexpr uint16_t ID_cpu_bltzal = 128;
constexpr uint16_t ID_cpu_bgezal = 129;
constexpr uint16_t ID_cpu_bltzall = 130;
constexpr uint16_t ID_cpu_bgezall = 131;
constexpr uint16_t ID_cpu_bal = 132;
constexpr uint16_t ID_cpu_mfc0 = 133;
constexpr uint16_t ID_cpu_dmfc0 = 134;
constexpr uint16_t ID_cpu_cfc0 = 135;
constexpr uint16_t ID_cpu_mtc0 = 136;
constexpr uint16_t ID_cpu_dmtc0 = 137;
constexpr uint16_t ID_cpu_ctc0 = 138;
constexpr uint16_t ID_cpu_bc0f = 139;
constexpr uint16_t ID_cpu_bc0t = 140;
constexpr uint16_t ID_cpu_bc0fl = 141;
constexpr uint16_t ID_cpu_bc0tl = 142;
constexpr uint16_t ID_cpu_tlbr = 143;
constexpr uint16_t ID_cpu_tlbwi = 144;
constexpr uint16_t ID_cpu_tlbwr = 145;
constexpr uint16_t ID_cpu_tlbp = 146;
constexpr uint16_t ID_cpu_rfe = 147;
constexpr uint16_t ID_cpu_eret = 148;
constexpr uint16_t ID_cpu_mfc1 = 149;
constexpr uint16_t ID_cpu_dmfc1 = 150;
constexpr uint16_t ID_cpu_mtc1 = 151;
constexpr uint16_t ID_cpu_dmtc1 = 152;
constexpr uint16_t ID_cpu_cfc1 = 153;
constexpr uint16_t ID_cpu_ctc1 = 154;
constexpr uint16_t ID_cpu_bc1f = 155;
constexpr uint16_t ID_cpu_bc1t = 156;
constexpr uint16_t ID_cpu_bc1fl = 157;
constexpr uint16_t ID_cpu_bc1tl = 158;
constexpr uint16_t ID_cpu_add_s = 159;
constexpr uint16_t ID_cpu_sub_s = 160;
constexpr uint16_t ID_cpu_mul_s = 161;
constexpr uint16_t ID_cpu_div_s = 162;
constexpr uint16_t ID_cpu_sqrt_s = 163;
constexpr uint16_t ID_cpu_abs_s = 164;
constexpr uint16_t ID_cpu_mov_s = 165;
constexpr uint16_t ID_cpu_neg_s = 166;
constexpr uint16_t ID_cpu_round_l_s = 167;
constexpr uint16_t ID_cpu_trunc_l_s = 168;
constexpr uint16_t ID_cpu_ceil_l_s = 169;
constexpr uint16_t ID_cpu_floor_l_s = 170;
constexpr uint16_t ID_cpu_round_w_s = 171;
constexpr uint16_t ID_cpu_trunc_w_s = 172;
constexpr uint16_t ID_cpu_ceil_w_s = 173;
constexpr uint16_t ID_cpu_floor_w_s = 174;
constexpr uint16_t ID_cpu_cvt_d_s = 175;
constexpr uint16_t ID_cpu_cvt_w_s = 176;
constexpr uint16_t ID_cpu_cvt_l_s = 177;
constexpr uint16_t ID_cpu_c_f_s = 178;
constexpr uint16_t ID_cpu_c_un_s = 179;
constexpr uint16_t ID_cpu_c_eq_s = 180;
constexpr uint16_t ID_cpu_c_ueq_s = 181;
constexpr uint16_t ID_cpu_c_olt_s = 182;
constexpr uint16_t ID_cpu_c_ult_s = 183;
constexpr uint16_t ID_cpu_c_ole_s = 184;
constexpr uint16_t ID_cpu_c_ule_s = 185;
constexpr uint16_t ID_cpu_c_sf_s = 186;
constexpr uint16_t ID_cpu_c_ngle_s = 187;
constexpr uint16_t ID_cpu_c_seq_s = 188;
constexpr uint16_t ID_cpu_c_ngl_s = 189;
constexpr uint16_t ID_cpu_c_lt_s = 190;
constexpr uint16_t ID_cpu_c_nge_s = 191;
constexpr uint16_t ID_cpu_c_le_s = 192;
constexpr uint16_t ID_cpu_c_ngt_s = 193;
constexpr uint16_t ID_cpu_add_d = 194;
constexpr uint16_t ID_cpu_sub_d = 195;
constexpr uint16_t ID_cpu_mul_d = 196;
constexpr uint16_t ID_cpu_div_d = 197;
constexpr uint16_t ID_cpu_sqrt_d = 198;
constexpr uint16_t ID_cpu_abs_d = 199;
constexpr uint16_t ID_cpu_mov_d = 200;
constexpr uint16_t ID_cpu_neg_d = 201;
constexpr uint16_t ID_cpu_round_l_d = 202;
constexpr uint16_t ID_cpu_trunc_l_d = 203;
constexpr uint16_t ID_cpu_ceil_l_d = 204;
constexpr uint16_t ID_cpu_floor_l_d = 205;
constexpr uint16_t ID_cpu_round_w_d = 206;
constexpr uint16_t ID_cpu_trunc_w_d = 207;
constexpr uint16_t ID_cpu_ceil_w_d = 208;
constexpr uint16_t ID_cpu_floor_w_d = 209;
constexpr uint16_t ID_cpu_cvt_s_d = 210;
constexpr uint16_t ID_cpu_cvt_w_d = 211;
constexpr uint16_t ID_cpu_cvt_l_d = 212;
constexpr uint16_t ID_cpu_c_f_d = 213;
constexpr uint16_t ID_cpu_c_un_d = 214;
constexpr uint16_t ID_cpu_c_eq_d = 215;
constexpr uint16_t ID_cpu_c_ueq_d = 216;
constexpr uint16_t ID_cpu_c_olt_d = 217;
constexpr uint16_t ID_cpu_c_ult_d = 218;
constexpr uint16_t ID_cpu_c_ole_d = 219;
constexpr uint16_t ID_cpu_c_ule_d = 220;
constexpr uint16_t ID_cpu_c_df_d = 221;
constexpr uint16_t ID_cpu_c_ngle_d = 222;
constexpr uint16_t ID_cpu_c_seq_d = 223;
constexpr uint16_t ID_cpu_c_ngl_d = 224;
constexpr uint16_t ID_cpu_c_lt_d = 225;
constexpr uint16_t ID_cpu_c_nge_d = 226;
constexpr uint16_t ID_cpu_c_le_d = 227;
constexpr uint16_t ID_cpu_c_ngt_d = 228;
constexpr uint16_t ID_cpu_cvt_s_w = 229;
constexpr uint16_t ID_cpu_cvt_d_w = 230;
constexpr uint16_t ID_cpu_cvt_s_l = 231;
constexpr uint16_t ID_cpu_cvt_d_l = 232;
constexpr uint16_t ID_cpu_mfc2 = 233;
constexpr uint16_t ID_cpu_mtc2 = 234;
constexpr uint16_t ID_cpu_cfc2 = 235;
constexpr uint16_t ID_cpu_ctc2 = 236;
constexpr uint16_t ID_r5900_INVALID = 237;
constexpr uint16_t ID_r5900_lq = 238;
constexpr uint16_t ID_r5900_sq = 239;
constexpr uint16_t ID_r5900_lqc2 = 240;
constexpr uint16_t ID_r5900_sqc2 = 241;
constexpr uint16_t ID_r5900_sync_p = 242;
constexpr uint16_t ID_r5900_mult = 243;
constexpr uint16_t ID_r5900_mfsa = 244;
constexpr uint16_t ID_r5900_mtsa = 245;
constexpr uint16_t ID_r5900_mtsab = 246;
constexpr uint16_t ID_r5900_mtsah = 247;
constexpr uint16_t ID_r5900_madd = 248;
constexpr uint16_t ID_r5900_maddu = 249;
constexpr uint16_t ID_r5900_plzcw = 250;
constexpr uint16_t ID_r5900_mfhi1 = 251;
constexpr uint16_t ID_r5900_mthi1 = 252;
constexpr uint16_t ID_r5900_mflo1 = 253;
constexpr uint16_t ID_r5900_mtlo1 = 254;
constexpr uint16_t ID_r5900_mult1 = 255;
constexpr uint16_t ID_r5900_multu1 = 256;
constexpr uint16_t ID_r5900_div1 = 257;
constexpr uint16_t ID_r5900_divu1 = 258;
constexpr uint16_t ID_r5900_madd1 = 259;
constexpr uint16_t ID_r5900_maddu1 = 260;
constexpr uint16_t ID_r5900_psllh = 261;
constexpr uint16_t ID_r5900_psrlh = 262;
constexpr uint16_t ID_r5900_psrah = 263;
constexpr uint16_t ID_r5900_psllw = 264;
constexpr uint16_t ID_r5900_psrlw = 265;
constexpr uint16_t ID_r5900_psraw = 266;
constexpr uint16_t ID_r5900_paddw = 267;
constexpr uint16_t ID_r5900_psubw = 268;
constexpr uint16_t ID_r5900_pcgtw = 269;
constexpr uint16_t ID_r5900_pmaxw = 270;
constexpr uint16_t ID_r5900_paddh = 271;
constexpr uint16_t ID_r5900_psubh = 272;
constexpr uint16_t ID_r5900_pcgth = 273;
constexpr uint16_t ID_r5900_pmaxh = 274;
constexpr uint16_t ID_r5900_paddb = 275;
constexpr uint16_t ID_r5900_psubb = 276;
constexpr uint16_t ID_r5900_pcgtb = 277;
constexpr uint16_t ID_r5900_paddsw = 278;
constexpr uint16_t ID_r5900_psubsw = 279;
constexpr uint16_t ID_r5900_pextlw = 280;
constexpr uint16_t ID_r5900_ppacw = 281;
constexpr uint16_t ID_r5900_paddsh = 282;
constexpr uint16_t ID_r5900_psubsh = 283;
constexpr uint16_t ID_r5900_pextlh = 284;
constexpr uint16_t ID_r5900_ppach = 285;
constexpr uint16_t ID_r5900_paddsb = 286;
constexpr uint16_t ID_r5900_psubsb = 287;
constexpr uint16_t ID_r5900_pextlb = 288;
constexpr uint16_t ID_r5900_ppacb = 289;
constexpr uint16_t ID_r5900_pext5 = 290;
constexpr uint16_t ID_r5900_ppac5 = 291;
constexpr uint16_t ID_r5900_pabsw = 292;
constexpr uint16_t ID_r5900_pceqw = 293;
constexpr uint16_t ID_r5900_pminw = 294;
constexpr uint16_t ID_r5900_padsbh = 295;
constexpr uint16_t ID_r5900_pabsh = 296;
constexpr uint16_t ID_r5900_pceqh = 297;
constexpr uint16_t ID_r5900_pminh = 298;
constexpr uint16_t ID_r5900_pceqb = 299;
constexpr uint16_t ID_r5900_padduw = 300;
constexpr uint16_t ID_r5900_psubuw = 301;
constexpr uint16_t ID_r5900_pextuw = 302;
constexpr uint16_t ID_r5900_padduh = 303;
constexpr uint16_t ID_r5900_psubuh = 304;
constexpr uint16_t ID_r5900_pextuh = 305;
constexpr uint16_t ID_r5900_paddub = 306;
constexpr uint16_t ID_r5900_psubub = 307;
constexpr uint16_t ID_r5900_pextub = 308;
constexpr uint16_t ID_r5900_qfsrv = 309;
constexpr uint16_t ID_r5900_pmaddw = 310;
constexpr uint16_t ID_r5900_psllvw = 311;
constexpr uint16_t ID_r5900_psrlvw = 312;
constexpr uint16_t ID_r5900_pmsubw = 313;
constexpr uint16_t ID_r5900_pmfhi = 314;
constexpr uint16_t ID_r5900_pmflo = 315;
constexpr uint16_t ID_r5900_pinth = 316;
constexpr uint16_t ID_r5900_pmultw = 317;
constexpr uint16_t ID_r5900_pdivw = 318;
constexpr uint16_t ID_r5900_pcpyld = 319;
constexpr uint16_t ID_r5900_pmaddh = 320;
constexpr uint16_t ID_r5900_phmadh = 321;
constexpr uint16_t ID_r5900_pand = 322;
constexpr uint16_t ID_r5900_pxor = 323;
constexpr uint16_t ID_r5900_pmsubh = 324;
constexpr uint16_t ID_r5900_phmsbh = 325;
constexpr uint16_t ID_r5900_pexeh = 326;
constexpr uint16_t ID_r5900_prevh = 327;
constexpr uint16_t ID_r5900_pmulth = 328;
constexpr uint16_t ID_r5900_pdivbw = 329;
constexpr uint16_t ID_r5900_pexew = 330;
constexpr uint16_t ID_r5900_prot3w = 331;
constexpr uint16_t ID_r5900_pmadduw = 332;
constexpr uint16_t ID_r5900_psravw = 333;
constexpr uint16_t ID_r5900_pmthi = 334;
constexpr uint16_t ID_r5900_pmtlo = 335;
constexpr uint16_t ID_r5900_pinteh = 336;
constexpr uint16_t ID_r5900_pmultuw = 337;
constexpr uint16_t ID_r5900_pdivuw = 338;
constexpr uint16_t ID_r5900_pcpyud = 339;
constexpr uint16_t ID_r5900_por = 340;
constexpr uint16_t ID_r5900_pnor = 341;
constexpr uint16_t ID_r5900_pexch = 342;
constexpr uint16_t ID_r5900_pcpyh = 343;
constexpr uint16_t ID_r5900_pexcw = 344;
constexpr uint16_t ID_r5900_pmfhl_lw = 345;
constexpr uint16_t ID_r5900_pmfhl_uw = 346;
constexpr uint16_t ID_r5900_pmfhl_slw = 347;
constexpr uint16_t ID_r5900_pmfhl_lh = 348;
constexpr uint16_t ID_r5900_pmfhl_sh = 349;
constexpr uint16_t ID_r5900_pmthl_lw = 350;
constexpr uint16_t ID_r5900_ei = 351;
constexpr uint16_t ID_r5900_di = 352;
constexpr uint16_t ID_r5900_c1__sqrt_s = 353;
constexpr uint16_t ID_r5900_rsqrt_s = 354;
constexpr uint16_t ID_r5900_adda_s = 355;
constexpr uint16_t ID_r5900_suba_s = 356;
constexpr uint16_t ID_r5900_mula_s = 357;
constexpr uint16_t ID_r5900_madd_s = 358;
constexpr uint16_t ID_r5900_msub_s = 359;
constexpr uint16_t ID_r5900_madda_s = 360;
constexpr uint16_t ID_r5900_msuba_s = 361;
constexpr uint16_t ID_r5900_max_s = 362;
constexpr uint16_t ID_r5900_min_s = 363;
constexpr uint16_t ID_r5900_c_lt_s = 364;
constexpr uint16_t ID_r5900_c_le_s = 365;
constexpr uint16_t ID_r5900_qmfc2_ni = 366;
constexpr uint16_t ID_r5900_cfc2_ni = 367;
constexpr uint16_t ID_r5900_qmtc2_ni = 368;
constexpr uint16_t ID_r5900_ctc2_ni = 369;
constexpr uint16_t ID_r5900_qmfc2_i = 370;
constexpr uint16_t ID_r5900_cfc2_i = 371;
constexpr uint16_t ID_r5900_qmtc2_i = 372;
constexpr uint16_t ID_r5900_ctc2_i = 373;
constexpr uint16_t ID_r5900_bc2f = 374;
constexpr uint16_t ID_r5900_bc2t = 375;
constexpr uint16_t ID_r5900_bc2fl = 376;
constexpr uint16_t ID_r5900_bc2tl = 377;
constexpr uint16_t ID_r5900_vaddx = 378;
constexpr uint16_t ID_r5900_vaddy = 379;
constexpr uint16_t ID_r5900_vaddz = 380;
constexpr uint16_t ID_r5900_vaddw = 381;
constexpr uint16_t ID_r5900_vsubx = 382;
constexpr uint16_t ID_r5900_vsuby = 383;
constexpr uint16_t ID_r5900_vsubz = 384;
constexpr uint16_t ID_r5900_vsubw = 385;
constexpr uint16_t ID_r5900_vmaddx = 386;
constexpr uint16_t ID_r5900_vmaddy = 387;
constexpr uint16_t ID_r5900_vmaddz = 388;
constexpr uint16_t ID_r5900_vmaddw = 389;
constexpr uint16_t ID_r5900_vmsubx = 390;
constexpr uint16_t ID_r5900_vmsuby = 391;
constexpr uint16_t ID_r5900_vmsubz = 392;
constexpr uint16_t ID_r5900_vmsubw = 393;
constexpr uint16_t ID_r5900_vmaxx = 394;
constexpr uint16_t ID_r5900_vmaxy = 395;
constexpr uint16_t ID_r5900_vmaxz = 396;
constexpr uint16_t ID_r5900_vmaxw = 397;
constexpr uint16_t ID_r5900_vminix = 398;
constexpr uint16_t ID_r5900_vminiy = 399;
constexpr uint16_t ID_r5900_vminiz = 400;
constexpr uint16_t ID_r5900_vminiw = 401;
constexpr uint16_t ID_r5900_vmulx = 402;
constexpr uint16_t ID_r5900_vmuly = 403;
constexpr uint16_t ID_r5900_vmulz = 404;
constexpr uint16_t ID_r5900_vmulw = 405;
constexpr uint16_t ID_r5900_vmulq = 406;
constexpr uint16_t ID_r5900_vmaxi = 407;
constexpr uint16_t ID_r5900_vmuli = 408;
constexpr uint16_t ID_r5900_vminii = 409;
constexpr uint16_t ID_r5900_vaddq = 410;
constexpr uint16_t ID_r5900_vmaddq = 411;
constexpr uint16_t ID_r5900_vaddi = 412;
constexpr uint16_t ID_r5900_vmaddi = 413;
constexpr uint16_t ID_r5900_vsubq = 414;
constexpr uint16_t ID_r5900_vmsubq = 415;
constexpr uint16_t ID_r5900_vsubi = 416;
constexpr uint16_t ID_r5900_vmsubi = 417;
constexpr uint16_t ID_r5900_vadd = 418;
constexpr uint16_t ID_r5900_vmadd = 419;
constexpr uint16_t ID_r5900_vmul = 420;
constexpr uint16_t ID_r5900_vmax = 421;
constexpr uint16_t ID_r5900_vsub = 422;
constexpr uint16_t ID_r5900_vmsub = 423;
constexpr uint16_t ID_r5900_vopmsub = 424;
constexpr uint16_t ID_r5900_vmini = 425;
constexpr uint16_t ID_r5900_viadd = 426;
constexpr uint16_t ID_r5900_visub = 427;
constexpr uint16_t ID_r5900_viaddi = 428;
constexpr uint16_t ID_r5900_viand = 429;
constexpr uint16_t ID_r5900_vior = 430;
constexpr uint16_t ID_r5900_vcallms = 431;
constexpr uint16_t ID_r5900_vcallmsr = 432;
constexpr uint16_t ID_r5900_vaddax = 433;
constexpr uint16_t ID_r5900_vadday = 434;
constexpr uint16_t ID_r5900_vaddaz = 435;
constexpr uint16_t ID_r5900_vaddaw = 436;
constexpr uint16_t ID_r5900_vsubax = 437;
constexpr uint16_t ID_r5900_vsubay = 438;
constexpr uint16_t ID_r5900_vsubaz = 439;
constexpr uint16_t ID_r5900_vsubaw = 440;
constexpr uint16_t ID_r5900_vmaddax = 441;
constexpr uint16_t ID_r5900_vmadday = 442;
constexpr uint16_t ID_r5900_vmaddaz = 443;
constexpr uint16_t ID_r5900_vmaddaw = 444;
constexpr uint16_t ID_r5900_vmsubax = 445;
constexpr uint16_t ID_r5900_vmsubay = 446;
constexpr uint16_t ID_r5900_vmsubaz = 447;
constexpr uint16_t ID_r5900_vmsubaw = 448;
constexpr uint16_t ID_r5900_vitof0 = 449;
constexpr uint16_t ID_r5900_vitof4 = 450;
constexpr uint16_t ID_r5900_vitof12 = 451;
constexpr uint16_t ID_r5900_vitof15 = 452;
constexpr uint16_t ID_r5900_vftoi0 = 453;
constexpr uint16_t ID_r5900_vftoi4 = 454;
constexpr uint16_t ID_r5900_vftoi12 = 455;
constexpr uint16_t ID_r5900_vftoi15 = 456;
constexpr uint16_t ID_r5900_vmulax = 457;
constexpr uint16_t ID_r5900_vmulay = 458;
constexpr uint16_t ID_r5900_vmulaz = 459;
constexpr uint16_t ID_r5900_vmulaw = 460;
constexpr uint16_t ID_r5900_vmulaq = 461;
constexpr uint16_t ID_r5900_vabs = 462;
constexpr uint16_t ID_r5900_vmulai = 463;
constexpr uint16_t ID_r5900_vclipw = 464;
constexpr uint16_t ID_r5900_vaddaq = 465;
constexpr uint16_t ID_r5900_vmaddaq = 466;
constexpr uint16_t ID_r5900_vaddai = 467;
constexpr uint16_t ID_r5900_vmaddai = 468;
constexpr uint16_t ID_r5900_vsubaq = 469;
constexpr uint16_t ID_r5900_vmsubaq = 470;
constexpr uint16_t ID_r5900_vsubai = 471;
constexpr uint16_t ID_r5900_vmsubai = 472;
constexpr uint16_t ID_r5900_vadda = 473;
constexpr uint16_t ID_r5900_vmadda = 474;
constexpr uint16_t ID_r5900_vmula = 475;
constexpr uint16_t ID_r5900_vsuba = 476;
constexpr uint16_t ID_r5900_vmsuba = 477;
constexpr uint16_t ID_r5900_vopmula = 478;
constexpr uint16_t ID_r5900_vnop = 479;
constexpr uint16_t ID_r5900_vmove = 480;
constexpr uint16_t ID_r5900_vmr32 = 481;
constexpr uint16_t ID_r5900_vlqi = 482;
constexpr uint16_t ID_r5900_vsqi = 483;
constexpr uint16_t ID_r5900_vlqd = 484;
constexpr uint16_t ID_r5900_vsqd = 485;
constexpr uint16_t ID_r5900_vdiv = 486;
constexpr uint16_t ID_r5900_vsqrt = 487;
constexpr uint16_t ID_r5900_vrsqrt = 488;
constexpr uint16_t ID_r5900_vwaitq = 489;
constexpr uint16_t ID_r5900_vmtir = 490;
constexpr uint16_t ID_r5900_vmfir = 491;
constexpr uint16_t ID_r5900_vrnext = 492;
constexpr uint16_t ID_r5900_vrget = 493;
constexpr uint16_t ID_r5900_vrinit = 494;
constexpr uint16_t ID_r5900_vrxor = 495;
constexpr uint16_t ID_r5900_vilwr_w = 496;
constexpr uint16_t ID_r5900_vilwr_z = 497;
constexpr uint16_t ID_r5900_vilwr_y = 498;
constexpr uint16_t ID_r5900_vilwr_x = 499;
constexpr uint16_t ID_r5900_viswr_w = 500;
constexpr uint16_t ID_r5900_viswr_z = 501;
constexpr uint16_t ID_r5900_viswr_y = 502;
constexpr uint16_t ID_r5900_viswr_x = 503;

const InstrDescriptor kDescriptors[] = {
    {"INVALID", {Operand::cpu_rs, Operand::cpu_rt, Operand::cpu_immediate}, false, false, 0}, // cpu_INVALID
    {"j", {Operand::cpu_label}, false, true, F_isJump}, // cpu_j
    {"jal", {Operand::cpu_label}, false, true, F_isJump | F_doesLink}, // cpu_jal
    {"beq", {Operand::cpu_rs, Operand::cpu_rt, Operand::cpu_branch_target_label}, false, true, F_isBranch}, // cpu_beq
    {"bne", {Operand::cpu_rs, Operand::cpu_rt, Operand::cpu_branch_target_label}, false, true, F_isBranch}, // cpu_bne
    {"beql", {Operand::cpu_rs, Operand::cpu_rt, Operand::cpu_branch_target_label}, false, true, F_isBranch}, // cpu_beql
    {"bnel", {Operand::cpu_rs, Operand::cpu_rt, Operand::cpu_branch_target_label}, false, true, F_isBranch}, // cpu_bnel
    {"blez", {Operand::cpu_rs, Operand::cpu_branch_target_label}, false, true, F_isBranch}, // cpu_blez
    {"blezl", {Operand::cpu_rs, Operand::cpu_branch_target_label}, false, true, F_isBranch}, // cpu_blezl
    {"bgtz", {Operand::cpu_rs, Operand::cpu_branch_target_label}, false, true, F_isBranch}, // cpu_bgtz
    {"bgtzl", {Operand::cpu_rs, Operand::cpu_branch_target_label}, false, true, F_isBranch}, // cpu_bgtzl
    {"addi", {Operand::cpu_rt, Operand::cpu_rs, Operand::cpu_immediate}, false, true, F_modifiesRt}, // cpu_addi
    {"addiu", {Operand::cpu_rt, Operand::cpu_rs, Operand::cpu_immediate}, false, true, F_modifiesRt}, // cpu_addiu
    {"slti", {Operand::cpu_rt, Operand::cpu_rs, Operand::cpu_immediate}, false, true, F_modifiesRt}, // cpu_slti
    {"sltiu", {Operand::cpu_rt, Operand::cpu_rs, Operand::cpu_immediate}, false, true, F_modifiesRt}, // cpu_sltiu
    {"andi", {Operand::cpu_rt, Operand::cpu_rs, Operand::cpu_immediate}, false, true, F_modifiesRt | F_isUnsigned}, // cpu_andi
    {"ori", {Operand::cpu_rt, Operand::cpu_rs, Operand::cpu_immediate}, false, true, F_modifiesRt | F_isUnsigned}, // cpu_ori
    {"xori", {Operand::cpu_rt, Operand::cpu_rs, Operand::cpu_immediate}, false, true, F_modifiesRt | F_isUnsigned}, // cpu_xori
    {"daddi", {Operand::cpu_rt, Operand::cpu_rs, Operand::cpu_immediate}, false, true, F_modifiesRt}, // cpu_daddi
    {"daddiu", {Operand::cpu_rt, Operand::cpu_rs, Operand::cpu_immediate}, false, true, F_modifiesRt}, // cpu_daddiu
    {"lui", {Operand::cpu_rt, Operand::cpu_immediate}, false, true, F_modifiesRt | F_isUnsigned}, // cpu_lui
    {"ldl", {Operand::cpu_rt, Operand::cpu_immediate_base}, false, true, F_doesLoad | F_modifiesRt}, // cpu_ldl
    {"ldr", {Operand::cpu_rt, Operand::cpu_immediate_base}, false, true, F_doesLoad | F_modifiesRt}, // cpu_ldr
    {"lb", {Operand::cpu_rt, Operand::cpu_immediate_base}, false, true, F_doesLoad | F_modifiesRt}, // cpu_lb
    {"lh", {Operand::cpu_rt, Operand::cpu_immediate_base}, false, true, F_doesLoad | F_modifiesRt}, // cpu_lh
    {"lwl", {Operand::cpu_rt, Operand::cpu_immediate_base}, false, true, F_doesLoad | F_modifiesRt}, // cpu_lwl
    {"lw", {Operand::cpu_rt, Operand::cpu_immediate_base}, false, true, F_doesLoad | F_modifiesRt}, // cpu_lw
    {"lbu", {Operand::cpu_rt, Operand::cpu_immediate_base}, false, true, F_doesLoad | F_modifiesRt}, // cpu_lbu
    {"lhu", {Operand::cpu_rt, Operand::cpu_immediate_base}, false, true, F_doesLoad | F_modifiesRt}, // cpu_lhu
    {"lwr", {Operand::cpu_rt, Operand::cpu_immediate_base}, false, true, F_doesLoad | F_modifiesRt}, // cpu_lwr
    {"lwu", {Operand::cpu_rt, Operand::cpu_immediate_base}, false, true, F_doesLoad | F_modifiesRt}, // cpu_lwu
    {"sb", {Operand::cpu_rt, Operand::cpu_immediate_base}, false, true, F_doesStore}, // cpu_sb
    {"sh", {Operand::cpu_rt, Operand::cpu_immediate_base}, false, true, F_doesStore}, // cpu_sh
    {"swl", {Operand::cpu_rt, Operand::cpu_immediate_base}, false, true, F_doesStore}, // cpu_swl
    {"sw", {Operand::cpu_rt, Operand::cpu_immediate_base}, false, true, F_doesStore}, // cpu_sw
    {"sdl", {Operand::cpu_rt, Operand::cpu_immediate_base}, false, true, F_doesStore}, // cpu_sdl
    {"sdr", {Operand::cpu_rt, Operand::cpu_immediate_base}, false, true, F_doesStore}, // cpu_sdr
    {"swr", {Operand::cpu_rt, Operand::cpu_immediate_base}, false, true, F_doesStore}, // cpu_swr
    {"ll", {Operand::cpu_rt, Operand::cpu_immediate_base}, false, true, F_doesLoad | F_modifiesRt}, // cpu_ll
    {"pref", {Operand::cpu_hint, Operand::cpu_immediate_base}, false, true, 0}, // cpu_pref
    {"lld", {Operand::cpu_rt, Operand::cpu_immediate_base}, false, true, F_doesLoad | F_modifiesRt}, // cpu_lld
    {"ld", {Operand::cpu_rt, Operand::cpu_immediate_base}, false, true, F_doesLoad | F_modifiesRt}, // cpu_ld
    {"sc", {Operand::cpu_rt, Operand::cpu_immediate_base}, false, true, F_doesStore}, // cpu_sc
    {"scd", {Operand::cpu_rt, Operand::cpu_immediate_base}, false, true, F_doesStore}, // cpu_scd
    {"sd", {Operand::cpu_rt, Operand::cpu_immediate_base}, false, true, F_doesStore}, // cpu_sd
    {"cache", {Operand::cpu_op, Operand::cpu_immediate_base}, false, true, 0}, // cpu_cache
    {"lwc1", {Operand::cpu_ft, Operand::cpu_immediate_base}, false, true, F_doesLoad | F_modifiesFt}, // cpu_lwc1
    {"ldc1", {Operand::cpu_ft, Operand::cpu_immediate_base}, false, true, F_doesLoad | F_modifiesFt}, // cpu_ldc1
    {"swc1", {Operand::cpu_ft, Operand::cpu_immediate_base}, false, true, F_doesStore}, // cpu_swc1
    {"sdc1", {Operand::cpu_ft, Operand::cpu_immediate_base}, false, true, F_doesStore}, // cpu_sdc1
    {"lwc2", {Operand::cpu_cop2t, Operand::cpu_immediate_base}, false, true, F_doesLoad}, // cpu_lwc2
    {"ldc2", {Operand::cpu_cop2t, Operand::cpu_immediate_base}, false, true, F_doesLoad}, // cpu_ldc2
    {"swc2", {Operand::cpu_cop2t, Operand::cpu_immediate_base}, false, true, F_doesStore}, // cpu_swc2
    {"sdc2", {Operand::cpu_cop2t, Operand::cpu_immediate_base}, false, true, F_doesStore}, // cpu_sdc2
    {"b", {Operand::cpu_branch_target_label}, false, true, F_isBranch}, // cpu_b
    {"beqz", {Operand::cpu_rs, Operand::cpu_branch_target_label}, false, true, F_isBranch}, // cpu_beqz
    {"bnez", {Operand::cpu_rs, Operand::cpu_branch_target_label}, false, true, F_isBranch}, // cpu_bnez
    {"sll", {Operand::cpu_rd, Operand::cpu_rt, Operand::cpu_sa}, false, true, F_modifiesRd}, // cpu_sll
    {"srl", {Operand::cpu_rd, Operand::cpu_rt, Operand::cpu_sa}, false, true, F_modifiesRd}, // cpu_srl
    {"sra", {Operand::cpu_rd, Operand::cpu_rt, Operand::cpu_sa}, false, true, F_modifiesRd}, // cpu_sra
    {"dsll", {Operand::cpu_rd, Operand::cpu_rt, Operand::cpu_sa}, false, true, F_modifiesRd}, // cpu_dsll
    {"dsrl", {Operand::cpu_rd, Operand::cpu_rt, Operand::cpu_sa}, false, true, F_modifiesRd}, // cpu_dsrl
    {"dsra", {Operand::cpu_rd, Operand::cpu_rt, Operand::cpu_sa}, false, true, F_modifiesRd}, // cpu_dsra
    {"dsll32", {Operand::cpu_rd, Operand::cpu_rt, Operand::cpu_sa}, false, true, F_modifiesRd}, // cpu_dsll32
    {"dsrl32", {Operand::cpu_rd, Operand::cpu_rt, Operand::cpu_sa}, false, true, F_modifiesRd}, // cpu_dsrl32
    {"dsra32", {Operand::cpu_rd, Operand::cpu_rt, Operand::cpu_sa}, false, true, F_modifiesRd}, // cpu_dsra32
    {"dsllv", {Operand::cpu_rd, Operand::cpu_rt, Operand::cpu_rs}, false, true, F_modifiesRd}, // cpu_dsllv
    {"dsrlv", {Operand::cpu_rd, Operand::cpu_rt, Operand::cpu_rs}, false, true, F_modifiesRd}, // cpu_dsrlv
    {"dsrav", {Operand::cpu_rd, Operand::cpu_rt, Operand::cpu_rs}, false, true, F_modifiesRd}, // cpu_dsrav
    {"sllv", {Operand::cpu_rd, Operand::cpu_rt, Operand::cpu_rs}, false, true, F_modifiesRd}, // cpu_sllv
    {"srlv", {Operand::cpu_rd, Operand::cpu_rt, Operand::cpu_rs}, false, true, F_modifiesRd}, // cpu_srlv
    {"srav", {Operand::cpu_rd, Operand::cpu_rt, Operand::cpu_rs}, false, true, F_modifiesRd}, // cpu_srav
    {"mthi", {Operand::cpu_rs}, false, true, F_modifiesHI}, // cpu_mthi
    {"mtlo", {Operand::cpu_rs}, false, true, F_modifiesLO}, // cpu_mtlo
    {"jr", {Operand::cpu_rs}, false, true, F_isJump}, // cpu_jr
    {"jalr", {Operand::cpu_maybe_rd_rs}, false, true, F_isJump | F_modifiesRd | F_doesLink}, // cpu_jalr
    {"mfhi", {Operand::cpu_rd}, false, true, F_modifiesRd}, // cpu_mfhi
    {"mflo", {Operand::cpu_rd}, false, true, F_modifiesRd}, // cpu_mflo
    {"movz", {Operand::cpu_rd, Operand::cpu_rs, Operand::cpu_rt}, false, true, F_modifiesRd}, // cpu_movz
    {"movn", {Operand::cpu_rd, Operand::cpu_rs, Operand::cpu_rt}, false, true, F_modifiesRd}, // cpu_movn
    {"div", {Operand::cpu_rd, Operand::cpu_rs, Operand::cpu_rt}, false, true, F_modifiesHI | F_modifiesLO}, // cpu_div
    {"divu", {Operand::cpu_rd, Operand::cpu_rs, Operand::cpu_rt}, false, true, F_modifiesHI | F_modifiesLO}, // cpu_divu
    {"div", {Operand::cpu_rs, Operand::cpu_rt}, false, true, F_modifiesHI | F_modifiesLO}, // cpu_sn64_div
    {"divu", {Operand::cpu_rs, Operand::cpu_rt}, false, true, F_modifiesHI | F_modifiesLO}, // cpu_sn64_divu
    {"ddiv", {Operand::cpu_rd, Operand::cpu_rs, Operand::cpu_rt}, false, true, F_modifiesHI | F_modifiesLO}, // cpu_ddiv
    {"ddivu", {Operand::cpu_rd, Operand::cpu_rs, Operand::cpu_rt}, false, true, F_modifiesHI | F_modifiesLO}, // cpu_ddivu
    {"add", {Operand::cpu_rd, Operand::cpu_rs, Operand::cpu_rt}, false, true, F_modifiesRd}, // cpu_add
    {"addu", {Operand::cpu_rd, Operand::cpu_rs, Operand::cpu_rt}, false, true, F_modifiesRd}, // cpu_addu
    {"sub", {Operand::cpu_rd, Operand::cpu_rs, Operand::cpu_rt}, false, true, F_modifiesRd}, // cpu_sub
    {"subu", {Operand::cpu_rd, Operand::cpu_rs, Operand::cpu_rt}, false, true, F_modifiesRd}, // cpu_subu
    {"and", {Operand::cpu_rd, Operand::cpu_rs, Operand::cpu_rt}, false, true, F_modifiesRd}, // cpu_and
    {"or", {Operand::cpu_rd, Operand::cpu_rs, Operand::cpu_rt}, false, true, F_modifiesRd}, // cpu_or
    {"xor", {Operand::cpu_rd, Operand::cpu_rs, Operand::cpu_rt}, false, true, F_modifiesRd}, // cpu_xor
    {"nor", {Operand::cpu_rd, Operand::cpu_rs, Operand::cpu_rt}, false, true, F_modifiesRd}, // cpu_nor
    {"slt", {Operand::cpu_rd, Operand::cpu_rs, Operand::cpu_rt}, false, true, F_modifiesRd}, // cpu_slt
    {"sltu", {Operand::cpu_rd, Operand::cpu_rs, Operand::cpu_rt}, false, true, F_modifiesRd}, // cpu_sltu
    {"dadd", {Operand::cpu_rd, Operand::cpu_rs, Operand::cpu_rt}, false, true, F_modifiesRd}, // cpu_dadd
    {"daddu", {Operand::cpu_rd, Operand::cpu_rs, Operand::cpu_rt}, false, true, F_modifiesRd}, // cpu_daddu
    {"dsub", {Operand::cpu_rd, Operand::cpu_rs, Operand::cpu_rt}, false, true, F_modifiesRd}, // cpu_dsub
    {"dsubu", {Operand::cpu_rd, Operand::cpu_rs, Operand::cpu_rt}, false, true, F_modifiesRd}, // cpu_dsubu
    {"syscall", {Operand::cpu_code_lower}, false, true, 0}, // cpu_syscall
    {"break", {Operand::cpu_code}, false, true, 0}, // cpu_break
    {"sync", {Operand::None}, false, true, 0}, // cpu_sync
    {"mult", {Operand::cpu_rs, Operand::cpu_rt}, false, true, F_modifiesHI | F_modifiesLO}, // cpu_mult
    {"multu", {Operand::cpu_rs, Operand::cpu_rt}, false, true, F_modifiesHI | F_modifiesLO}, // cpu_multu
    {"dmult", {Operand::cpu_rs, Operand::cpu_rt}, false, true, F_modifiesHI | F_modifiesLO}, // cpu_dmult
    {"dmultu", {Operand::cpu_rs, Operand::cpu_rt}, false, true, F_modifiesHI | F_modifiesLO}, // cpu_dmultu
    {"tge", {Operand::cpu_rs, Operand::cpu_rt, Operand::cpu_code_lower}, false, true, F_isTrap}, // cpu_tge
    {"tgeu", {Operand::cpu_rs, Operand::cpu_rt, Operand::cpu_code_lower}, false, true, F_isTrap}, // cpu_tgeu
    {"tlt", {Operand::cpu_rs, Operand::cpu_rt, Operand::cpu_code_lower}, false, true, F_isTrap}, // cpu_tlt
    {"tltu", {Operand::cpu_rs, Operand::cpu_rt, Operand::cpu_code_lower}, false, true, F_isTrap}, // cpu_tltu
    {"teq", {Operand::cpu_rs, Operand::cpu_rt, Operand::cpu_code_lower}, false, true, F_isTrap}, // cpu_teq
    {"tne", {Operand::cpu_rs, Operand::cpu_rt, Operand::cpu_code_lower}, false, true, F_isTrap}, // cpu_tne
    {"nop", {Operand::None}, false, true, 0}, // cpu_nop
    {"move", {Operand::cpu_rd, Operand::cpu_rs}, false, true, F_modifiesRd}, // cpu_move
    {"not", {Operand::cpu_rd, Operand::cpu_rs}, false, true, F_modifiesRd}, // cpu_not
    {"neg", {Operand::cpu_rd, Operand::cpu_rt}, false, true, F_modifiesRd}, // cpu_neg
    {"negu", {Operand::cpu_rd, Operand::cpu_rt}, false, true, F_modifiesRd}, // cpu_negu
    {"bltz", {Operand::cpu_rs, Operand::cpu_branch_target_label}, false, true, F_isBranch}, // cpu_bltz
    {"bgez", {Operand::cpu_rs, Operand::cpu_branch_target_label}, false, true, F_isBranch}, // cpu_bgez
    {"bltzl", {Operand::cpu_rs, Operand::cpu_branch_target_label}, false, true, F_isBranch}, // cpu_bltzl
    {"bgezl", {Operand::cpu_rs, Operand::cpu_branch_target_label}, false, true, F_isBranch}, // cpu_bgezl
    {"tgei", {Operand::cpu_rs, Operand::cpu_immediate}, false, true, F_isTrap}, // cpu_tgei
    {"tgeiu", {Operand::cpu_rs, Operand::cpu_immediate}, false, true, F_isTrap}, // cpu_tgeiu
    {"tlti", {Operand::cpu_rs, Operand::cpu_immediate}, false, true, F_isTrap}, // cpu_tlti
    {"tltiu", {Operand::cpu_rs, Operand::cpu_immediate}, false, true, F_isTrap}, // cpu_tltiu
    {"teqi", {Operand::cpu_rs, Operand::cpu_immediate}, false, true, F_isTrap}, // cpu_teqi
    {"tnei", {Operand::cpu_rs, Operand::cpu_immediate}, false, true, F_isTrap}, // cpu_tnei
    {"bltzal", {Operand::cpu_rs, Operand::cpu_branch_target_label}, false, true, F_isBranch | F_doesLink}, // cpu_bltzal
    {"bgezal", {Operand::cpu_rs, Operand::cpu_branch_target_label}, false, true, F_isBranch | F_doesLink}, // cpu_bgezal
    {"bltzall", {Operand::cpu_rs, Operand::cpu_branch_target_label}, false, true, F_isBranch | F_doesLink}, // cpu_bltzall
    {"bgezall", {Operand::cpu_rs, Operand::cpu_branch_target_label}, false, true, F_isBranch | F_doesLink}, // cpu_bgezall
    {"bal", {Operand::cpu_branch_target_label}, false, true, F_isBranch | F_doesLink}, // cpu_bal
    {"mfc0", {Operand::cpu_rt, Operand::cpu_cop0d}, false, true, F_modifiesRt}, // cpu_mfc0
    {"dmfc0", {Operand::cpu_rt, Operand::cpu_cop0d}, false, true, F_modifiesRt}, // cpu_dmfc0
    {"cfc0", {Operand::cpu_rt, Operand::cpu_cop0d}, false, true, F_modifiesRt}, // cpu_cfc0
    {"mtc0", {Operand::cpu_rt, Operand::cpu_cop0d}, false, true, 0}, // cpu_mtc0
    {"dmtc0", {Operand::cpu_rt, Operand::cpu_cop0d}, false, true, 0}, // cpu_dmtc0
    {"ctc0", {Operand::cpu_rt, Operand::cpu_cop0d}, false, true, 0}, // cpu_ctc0
    {"bc0f", {Operand::cpu_branch_target_label}, false, true, F_isBranch}, // cpu_bc0f
    {"bc0t", {Operand::cpu_branch_target_label}, false, true, F_isBranch}, // cpu_bc0t
    {"bc0fl", {Operand::cpu_branch_target_label}, false, true, F_isBranch}, // cpu_bc0fl
    {"bc0tl", {Operand::cpu_branch_target_label}, false, true, F_isBranch}, // cpu_bc0tl
    {"tlbr", {Operand::None}, false, true, 0}, // cpu_tlbr
    {"tlbwi", {Operand::None}, false, true, 0}, // cpu_tlbwi
    {"tlbwr", {Operand::None}, false, true, 0}, // cpu_tlbwr
    {"tlbp", {Operand::None}, false, true, 0}, // cpu_tlbp
    {"rfe", {Operand::None}, false, true, 0}, // cpu_rfe
    {"eret", {Operand::None}, false, true, 0}, // cpu_eret
    {"mfc1", {Operand::cpu_rt, Operand::cpu_fs}, false, true, F_modifiesRt}, // cpu_mfc1
    {"dmfc1", {Operand::cpu_rt, Operand::cpu_fs}, false, true, F_modifiesRt}, // cpu_dmfc1
    {"mtc1", {Operand::cpu_rt, Operand::cpu_fs}, false, true, F_modifiesFs}, // cpu_mtc1
    {"dmtc1", {Operand::cpu_rt, Operand::cpu_fs}, false, true, F_modifiesFs}, // cpu_dmtc1
    {"cfc1", {Operand::cpu_rt, Operand::cpu_cop1cs}, false, true, F_modifiesRt}, // cpu_cfc1
    {"ctc1", {Operand::cpu_rt, Operand::cpu_cop1cs}, false, true, 0}, // cpu_ctc1
    {"bc1f", {Operand::cpu_branch_target_label}, false, true, F_isBranch}, // cpu_bc1f
    {"bc1t", {Operand::cpu_branch_target_label}, false, true, F_isBranch}, // cpu_bc1t
    {"bc1fl", {Operand::cpu_branch_target_label}, false, true, F_isBranch}, // cpu_bc1fl
    {"bc1tl", {Operand::cpu_branch_target_label}, false, true, F_isBranch}, // cpu_bc1tl
    {"add.s", {Operand::cpu_fd, Operand::cpu_fs, Operand::cpu_ft}, false, true, F_modifiesFd}, // cpu_add_s
    {"sub.s", {Operand::cpu_fd, Operand::cpu_fs, Operand::cpu_ft}, false, true, F_modifiesFd}, // cpu_sub_s
    {"mul.s", {Operand::cpu_fd, Operand::cpu_fs, Operand::cpu_ft}, false, true, F_modifiesFd}, // cpu_mul_s
    {"div.s", {Operand::cpu_fd, Operand::cpu_fs, Operand::cpu_ft}, false, true, F_modifiesFd}, // cpu_div_s
    {"sqrt.s", {Operand::cpu_fd, Operand::cpu_fs}, false, true, F_modifiesFd}, // cpu_sqrt_s
    {"abs.s", {Operand::cpu_fd, Operand::cpu_fs}, false, true, F_modifiesFd}, // cpu_abs_s
    {"mov.s", {Operand::cpu_fd, Operand::cpu_fs}, false, true, F_modifiesFd}, // cpu_mov_s
    {"neg.s", {Operand::cpu_fd, Operand::cpu_fs}, false, true, F_modifiesFd}, // cpu_neg_s
    {"round.l.s", {Operand::cpu_fd, Operand::cpu_fs}, false, true, F_modifiesFd}, // cpu_round_l_s
    {"trunc.l.s", {Operand::cpu_fd, Operand::cpu_fs}, false, true, F_modifiesFd}, // cpu_trunc_l_s
    {"ceil.l.s", {Operand::cpu_fd, Operand::cpu_fs}, false, true, F_modifiesFd}, // cpu_ceil_l_s
    {"floor.l.s", {Operand::cpu_fd, Operand::cpu_fs}, false, true, F_modifiesFd}, // cpu_floor_l_s
    {"round.w.s", {Operand::cpu_fd, Operand::cpu_fs}, false, true, F_modifiesFd}, // cpu_round_w_s
    {"trunc.w.s", {Operand::cpu_fd, Operand::cpu_fs}, false, true, F_modifiesFd}, // cpu_trunc_w_s
    {"ceil.w.s", {Operand::cpu_fd, Operand::cpu_fs}, false, true, F_modifiesFd}, // cpu_ceil_w_s
    {"floor.w.s", {Operand::cpu_fd, Operand::cpu_fs}, false, true, F_modifiesFd}, // cpu_floor_w_s
    {"cvt.d.s", {Operand::cpu_fd, Operand::cpu_fs}, false, true, F_modifiesFd}, // cpu_cvt_d_s
    {"cvt.w.s", {Operand::cpu_fd, Operand::cpu_fs}, false, true, F_modifiesFd}, // cpu_cvt_w_s
    {"cvt.l.s", {Operand::cpu_fd, Operand::cpu_fs}, false, true, F_modifiesFd}, // cpu_cvt_l_s
    {"c.f.s", {Operand::cpu_fs, Operand::cpu_ft}, false, true, 0}, // cpu_c_f_s
    {"c.un.s", {Operand::cpu_fs, Operand::cpu_ft}, false, true, 0}, // cpu_c_un_s
    {"c.eq.s", {Operand::cpu_fs, Operand::cpu_ft}, false, true, 0}, // cpu_c_eq_s
    {"c.ueq.s", {Operand::cpu_fs, Operand::cpu_ft}, false, true, 0}, // cpu_c_ueq_s
    {"c.olt.s", {Operand::cpu_fs, Operand::cpu_ft}, false, true, 0}, // cpu_c_olt_s
    {"c.ult.s", {Operand::cpu_fs, Operand::cpu_ft}, false, true, 0}, // cpu_c_ult_s
    {"c.ole.s", {Operand::cpu_fs, Operand::cpu_ft}, false, true, 0}, // cpu_c_ole_s
    {"c.ule.s", {Operand::cpu_fs, Operand::cpu_ft}, false, true, 0}, // cpu_c_ule_s
    {"c.sf.s", {Operand::cpu_fs, Operand::cpu_ft}, false, true, 0}, // cpu_c_sf_s
    {"c.ngle.s", {Operand::cpu_fs, Operand::cpu_ft}, false, true, 0}, // cpu_c_ngle_s
    {"c.seq.s", {Operand::cpu_fs, Operand::cpu_ft}, false, true, 0}, // cpu_c_seq_s
    {"c.ngl.s", {Operand::cpu_fs, Operand::cpu_ft}, false, true, 0}, // cpu_c_ngl_s
    {"c.lt.s", {Operand::cpu_fs, Operand::cpu_ft}, false, true, 0}, // cpu_c_lt_s
    {"c.nge.s", {Operand::cpu_fs, Operand::cpu_ft}, false, true, 0}, // cpu_c_nge_s
    {"c.le.s", {Operand::cpu_fs, Operand::cpu_ft}, false, true, 0}, // cpu_c_le_s
    {"c.ngt.s", {Operand::cpu_fs, Operand::cpu_ft}, false, true, 0}, // cpu_c_ngt_s
    {"add.d", {Operand::cpu_fd, Operand::cpu_fs, Operand::cpu_ft}, false, true, F_modifiesFd}, // cpu_add_d
    {"sub.d", {Operand::cpu_fd, Operand::cpu_fs, Operand::cpu_ft}, false, true, F_modifiesFd}, // cpu_sub_d
    {"mul.d", {Operand::cpu_fd, Operand::cpu_fs, Operand::cpu_ft}, false, true, F_modifiesFd}, // cpu_mul_d
    {"div.d", {Operand::cpu_fd, Operand::cpu_fs, Operand::cpu_ft}, false, true, F_modifiesFd}, // cpu_div_d
    {"sqrt.d", {Operand::cpu_fd, Operand::cpu_fs}, false, true, F_modifiesFd}, // cpu_sqrt_d
    {"abs.d", {Operand::cpu_fd, Operand::cpu_fs}, false, true, F_modifiesFd}, // cpu_abs_d
    {"mov.d", {Operand::cpu_fd, Operand::cpu_fs}, false, true, F_modifiesFd}, // cpu_mov_d
    {"neg.d", {Operand::cpu_fd, Operand::cpu_fs}, false, true, F_modifiesFd}, // cpu_neg_d
    {"round.l.d", {Operand::cpu_fd, Operand::cpu_fs}, false, true, F_modifiesFd}, // cpu_round_l_d
    {"trunc.l.d", {Operand::cpu_fd, Operand::cpu_fs}, false, true, F_modifiesFd}, // cpu_trunc_l_d
    {"ceil.l.d", {Operand::cpu_fd, Operand::cpu_fs}, false, true, F_modifiesFd}, // cpu_ceil_l_d
    {"floor.l.d", {Operand::cpu_fd, Operand::cpu_fs}, false, true, F_modifiesFd}, // cpu_floor_l_d
    {"round.w.d", {Operand::cpu_fd, Operand::cpu_fs}, false, true, F_modifiesFd}, // cpu_round_w_d
    {"trunc.w.d", {Operand::cpu_fd, Operand::cpu_fs}, false, true, F_modifiesFd}, // cpu_trunc_w_d
    {"ceil.w.d", {Operand::cpu_fd, Operand::cpu_fs}, false, true, F_modifiesFd}, // cpu_ceil_w_d
    {"floor.w.d", {Operand::cpu_fd, Operand::cpu_fs}, false, true, F_modifiesFd}, // cpu_floor_w_d
    {"cvt.s.d", {Operand::cpu_fd, Operand::cpu_fs}, false, true, F_modifiesFd}, // cpu_cvt_s_d
    {"cvt.w.d", {Operand::cpu_fd, Operand::cpu_fs}, false, true, F_modifiesFd}, // cpu_cvt_w_d
    {"cvt.l.d", {Operand::cpu_fd, Operand::cpu_fs}, false, true, F_modifiesFd}, // cpu_cvt_l_d
    {"c.f.d", {Operand::cpu_fs, Operand::cpu_ft}, false, true, 0}, // cpu_c_f_d
    {"c.un.d", {Operand::cpu_fs, Operand::cpu_ft}, false, true, 0}, // cpu_c_un_d
    {"c.eq.d", {Operand::cpu_fs, Operand::cpu_ft}, false, true, 0}, // cpu_c_eq_d
    {"c.ueq.d", {Operand::cpu_fs, Operand::cpu_ft}, false, true, 0}, // cpu_c_ueq_d
    {"c.olt.d", {Operand::cpu_fs, Operand::cpu_ft}, false, true, 0}, // cpu_c_olt_d
    {"c.ult.d", {Operand::cpu_fs, Operand::cpu_ft}, false, true, 0}, // cpu_c_ult_d
    {"c.ole.d", {Operand::cpu_fs, Operand::cpu_ft}, false, true, 0}, // cpu_c_ole_d
    {"c.ule.d", {Operand::cpu_fs, Operand::cpu_ft}, false, true, 0}, // cpu_c_ule_d
    {"c.df.d", {Operand::cpu_fs, Operand::cpu_ft}, false, true, 0}, // cpu_c_df_d
    {"c.ngle.d", {Operand::cpu_fs, Operand::cpu_ft}, false, true, 0}, // cpu_c_ngle_d
    {"c.seq.d", {Operand::cpu_fs, Operand::cpu_ft}, false, true, 0}, // cpu_c_seq_d
    {"c.ngl.d", {Operand::cpu_fs, Operand::cpu_ft}, false, true, 0}, // cpu_c_ngl_d
    {"c.lt.d", {Operand::cpu_fs, Operand::cpu_ft}, false, true, 0}, // cpu_c_lt_d
    {"c.nge.d", {Operand::cpu_fs, Operand::cpu_ft}, false, true, 0}, // cpu_c_nge_d
    {"c.le.d", {Operand::cpu_fs, Operand::cpu_ft}, false, true, 0}, // cpu_c_le_d
    {"c.ngt.d", {Operand::cpu_fs, Operand::cpu_ft}, false, true, 0}, // cpu_c_ngt_d
    {"cvt.s.w", {Operand::cpu_fd, Operand::cpu_fs}, false, true, F_modifiesFd}, // cpu_cvt_s_w
    {"cvt.d.w", {Operand::cpu_fd, Operand::cpu_fs}, false, true, F_modifiesFd}, // cpu_cvt_d_w
    {"cvt.s.l", {Operand::cpu_fd, Operand::cpu_fs}, false, true, F_modifiesFd}, // cpu_cvt_s_l
    {"cvt.d.l", {Operand::cpu_fd, Operand::cpu_fs}, false, true, F_modifiesFd}, // cpu_cvt_d_l
    {"mfc2", {Operand::cpu_rt, Operand::cpu_cop2cd}, false, true, F_modifiesRt}, // cpu_mfc2
    {"mtc2", {Operand::cpu_rt, Operand::cpu_cop2cd}, false, true, 0}, // cpu_mtc2
    {"cfc2", {Operand::cpu_rt, Operand::cpu_cop2cd}, false, true, F_modifiesRt}, // cpu_cfc2
    {"ctc2", {Operand::cpu_rt, Operand::cpu_cop2cd}, false, true, 0}, // cpu_ctc2
    {"INVALID", {Operand::cpu_rs, Operand::cpu_rt, Operand::cpu_immediate}, false, false, 0}, // r5900_INVALID
    {"lq", {Operand::cpu_rt, Operand::cpu_immediate_base}, false, true, F_doesLoad | F_modifiesRt}, // r5900_lq
    {"sq", {Operand::cpu_rt, Operand::cpu_immediate_base}, false, true, F_doesStore}, // r5900_sq
    {"lqc2", {Operand::r5900_vft, Operand::cpu_immediate_base}, false, true, F_doesLoad}, // r5900_lqc2
    {"sqc2", {Operand::r5900_vft, Operand::cpu_immediate_base}, false, true, F_doesStore}, // r5900_sqc2
    {"sync.p", {Operand::None}, false, true, 0}, // r5900_sync_p
    {"mult", {Operand::cpu_rd, Operand::cpu_rs, Operand::cpu_rt}, false, true, F_modifiesRd}, // r5900_mult
    {"mfsa", {Operand::cpu_rd}, false, true, F_modifiesRd}, // r5900_mfsa
    {"mtsa", {Operand::cpu_rs}, false, true, 0}, // r5900_mtsa
    {"mtsab", {Operand::cpu_rs, Operand::cpu_immediate}, false, true, 0}, // r5900_mtsab
    {"mtsah", {Operand::cpu_rs, Operand::cpu_immediate}, false, true, 0}, // r5900_mtsah
    {"madd", {Operand::cpu_rd, Operand::cpu_rs, Operand::cpu_rt}, false, true, F_modifiesRd}, // r5900_madd
    {"maddu", {Operand::cpu_rd, Operand::cpu_rs, Operand::cpu_rt}, false, true, F_modifiesRd}, // r5900_maddu
    {"plzcw", {Operand::cpu_rd, Operand::cpu_rs}, false, true, F_modifiesRd}, // r5900_plzcw
    {"mfhi1", {Operand::cpu_rd}, false, true, F_modifiesRd}, // r5900_mfhi1
    {"mthi1", {Operand::cpu_rs}, false, true, 0}, // r5900_mthi1
    {"mflo1", {Operand::cpu_rd}, false, true, 0}, // r5900_mflo1
    {"mtlo1", {Operand::cpu_rs}, false, true, 0}, // r5900_mtlo1
    {"mult1", {Operand::cpu_rd, Operand::cpu_rs, Operand::cpu_rt}, false, true, F_modifiesRd}, // r5900_mult1
    {"multu1", {Operand::cpu_rd, Operand::cpu_rs, Operand::cpu_rt}, false, true, F_modifiesRd}, // r5900_multu1
    {"div1", {Operand::cpu_zero, Operand::cpu_rs, Operand::cpu_rt}, false, true, 0}, // r5900_div1
    {"divu1", {Operand::cpu_zero, Operand::cpu_rs, Operand::cpu_rt}, false, true, 0}, // r5900_divu1
    {"madd1", {Operand::cpu_rd, Operand::cpu_rs, Operand::cpu_rt}, false, true, F_modifiesRd}, // r5900_madd1
    {"maddu1", {Operand::cpu_rd, Operand::cpu_rs, Operand::cpu_rt}, false, true, F_modifiesRd}, // r5900_maddu1
    {"psllh", {Operand::cpu_rd, Operand::cpu_rt, Operand::cpu_sa}, false, true, F_modifiesRd}, // r5900_psllh
    {"psrlh", {Operand::cpu_rd, Operand::cpu_rt, Operand::cpu_sa}, false, true, F_modifiesRd}, // r5900_psrlh
    {"psrah", {Operand::cpu_rd, Operand::cpu_rt, Operand::cpu_sa}, false, true, F_modifiesRd}, // r5900_psrah
    {"psllw", {Operand::cpu_rd, Operand::cpu_rt, Operand::cpu_sa}, false, true, F_modifiesRd}, // r5900_psllw
    {"psrlw", {Operand::cpu_rd, Operand::cpu_rt, Operand::cpu_sa}, false, true, F_modifiesRd}, // r5900_psrlw
    {"psraw", {Operand::cpu_rd, Operand::cpu_rt, Operand::cpu_sa}, false, true, F_modifiesRd}, // r5900_psraw
    {"paddw", {Operand::cpu_rd, Operand::cpu_rs, Operand::cpu_rt}, false, true, F_modifiesRd}, // r5900_paddw
    {"psubw", {Operand::cpu_rd, Operand::cpu_rs, Operand::cpu_rt}, false, true, F_modifiesRd}, // r5900_psubw
    {"pcgtw", {Operand::cpu_rd, Operand::cpu_rs, Operand::cpu_rt}, false, true, F_modifiesRd}, // r5900_pcgtw
    {"pmaxw", {Operand::cpu_rd, Operand::cpu_rs, Operand::cpu_rt}, false, true, F_modifiesRd}, // r5900_pmaxw
    {"paddh", {Operand::cpu_rd, Operand::cpu_rs, Operand::cpu_rt}, false, true, F_modifiesRd}, // r5900_paddh
    {"psubh", {Operand::cpu_rd, Operand::cpu_rs, Operand::cpu_rt}, false, true, F_modifiesRd}, // r5900_psubh
    {"pcgth", {Operand::cpu_rd, Operand::cpu_rs, Operand::cpu_rt}, false, true, F_modifiesRd}, // r5900_pcgth
    {"pmaxh", {Operand::cpu_rd, Operand::cpu_rs, Operand::cpu_rt}, false, true, F_modifiesRd}, // r5900_pmaxh
    {"paddb", {Operand::cpu_rd, Operand::cpu_rs, Operand::cpu_rt}, false, true, F_modifiesRd}, // r5900_paddb
    {"psubb", {Operand::cpu_rd, Operand::cpu_rs, Operand::cpu_rt}, false, true, F_modifiesRd}, // r5900_psubb
    {"pcgtb", {Operand::cpu_rd, Operand::cpu_rs, Operand::cpu_rt}, false, true, F_modifiesRd}, // r5900_pcgtb
    {"paddsw", {Operand::cpu_rd, Operand::cpu_rs, Operand::cpu_rt}, false, true, F_modifiesRd}, // r5900_paddsw
    {"psubsw", {Operand::cpu_rd, Operand::cpu_rs, Operand::cpu_rt}, false, true, F_modifiesRd}, // r5900_psubsw
    {"pextlw", {Operand::cpu_rd, Operand::cpu_rs, Operand::cpu_rt}, false, true, F_modifiesRd}, // r5900_pextlw
    {"ppacw", {Operand::cpu_rd, Operand::cpu_rs, Operand::cpu_rt}, false, true, F_modifiesRd}, // r5900_ppacw
    {"paddsh", {Operand::cpu_rd, Operand::cpu_rs, Operand::cpu_rt}, false, true, F_modifiesRd}, // r5900_paddsh
    {"psubsh", {Operand::cpu_rd, Operand::cpu_rs, Operand::cpu_rt}, false, true, F_modifiesRd}, // r5900_psubsh
    {"pextlh", {Operand::cpu_rd, Operand::cpu_rs, Operand::cpu_rt}, false, true, F_modifiesRd}, // r5900_pextlh
    {"ppach", {Operand::cpu_rd, Operand::cpu_rs, Operand::cpu_rt}, false, true, F_modifiesRd}, // r5900_ppach
    {"paddsb", {Operand::cpu_rd, Operand::cpu_rs, Operand::cpu_rt}, false, true, F_modifiesRd}, // r5900_paddsb
    {"psubsb", {Operand::cpu_rd, Operand::cpu_rs, Operand::cpu_rt}, false, true, F_modifiesRd}, // r5900_psubsb
    {"pextlb", {Operand::cpu_rd, Operand::cpu_rs, Operand::cpu_rt}, false, true, F_modifiesRd}, // r5900_pextlb
    {"ppacb", {Operand::cpu_rd, Operand::cpu_rs, Operand::cpu_rt}, false, true, F_modifiesRd}, // r5900_ppacb
    {"pext5", {Operand::cpu_rd, Operand::cpu_rt}, false, true, F_modifiesRd}, // r5900_pext5
    {"ppac5", {Operand::cpu_rd, Operand::cpu_rt}, false, true, F_modifiesRd}, // r5900_ppac5
    {"pabsw", {Operand::cpu_rd, Operand::cpu_rt}, false, true, F_modifiesRd}, // r5900_pabsw
    {"pceqw", {Operand::cpu_rd, Operand::cpu_rs, Operand::cpu_rt}, false, true, F_modifiesRd}, // r5900_pceqw
    {"pminw", {Operand::cpu_rd, Operand::cpu_rs, Operand::cpu_rt}, false, true, F_modifiesRd}, // r5900_pminw
    {"padsbh", {Operand::cpu_rd, Operand::cpu_rs, Operand::cpu_rt}, false, true, F_modifiesRd}, // r5900_padsbh
    {"pabsh", {Operand::cpu_rd, Operand::cpu_rt}, false, true, F_modifiesRd}, // r5900_pabsh
    {"pceqh", {Operand::cpu_rd, Operand::cpu_rs, Operand::cpu_rt}, false, true, F_modifiesRd}, // r5900_pceqh
    {"pminh", {Operand::cpu_rd, Operand::cpu_rs, Operand::cpu_rt}, false, true, F_modifiesRd}, // r5900_pminh
    {"pceqb", {Operand::cpu_rd, Operand::cpu_rs, Operand::cpu_rt}, false, true, F_modifiesRd}, // r5900_pceqb
    {"padduw", {Operand::cpu_rd, Operand::cpu_rs, Operand::cpu_rt}, false, true, F_modifiesRd}, // r5900_padduw
    {"psubuw", {Operand::cpu_rd, Operand::cpu_rs, Operand::cpu_rt}, false, true, F_modifiesRd}, // r5900_psubuw
    {"pextuw", {Operand::cpu_rd, Operand::cpu_rs, Operand::cpu_rt}, false, true, F_modifiesRd}, // r5900_pextuw
    {"padduh", {Operand::cpu_rd, Operand::cpu_rs, Operand::cpu_rt}, false, true, F_modifiesRd}, // r5900_padduh
    {"psubuh", {Operand::cpu_rd, Operand::cpu_rs, Operand::cpu_rt}, false, true, F_modifiesRd}, // r5900_psubuh
    {"pextuh", {Operand::cpu_rd, Operand::cpu_rs, Operand::cpu_rt}, false, true, F_modifiesRd}, // r5900_pextuh
    {"paddub", {Operand::cpu_rd, Operand::cpu_rs, Operand::cpu_rt}, false, true, F_modifiesRd}, // r5900_paddub
    {"psubub", {Operand::cpu_rd, Operand::cpu_rs, Operand::cpu_rt}, false, true, F_modifiesRd}, // r5900_psubub
    {"pextub", {Operand::cpu_rd, Operand::cpu_rs, Operand::cpu_rt}, false, true, F_modifiesRd}, // r5900_pextub
    {"qfsrv", {Operand::cpu_rd, Operand::cpu_rs, Operand::cpu_rt}, false, true, F_modifiesRd}, // r5900_qfsrv
    {"pmaddw", {Operand::cpu_rd, Operand::cpu_rs, Operand::cpu_rt}, false, true, F_modifiesRd}, // r5900_pmaddw
    {"psllvw", {Operand::cpu_rd, Operand::cpu_rt, Operand::cpu_rs}, false, true, F_modifiesRd}, // r5900_psllvw
    {"psrlvw", {Operand::cpu_rd, Operand::cpu_rt, Operand::cpu_rs}, false, true, F_modifiesRd}, // r5900_psrlvw
    {"pmsubw", {Operand::cpu_rd, Operand::cpu_rs, Operand::cpu_rt}, false, true, F_modifiesRd}, // r5900_pmsubw
    {"pmfhi", {Operand::cpu_rd}, false, true, F_modifiesRd}, // r5900_pmfhi
    {"pmflo", {Operand::cpu_rd}, false, true, F_modifiesRd}, // r5900_pmflo
    {"pinth", {Operand::cpu_rd, Operand::cpu_rs, Operand::cpu_rt}, false, true, F_modifiesRd}, // r5900_pinth
    {"pmultw", {Operand::cpu_rd, Operand::cpu_rs, Operand::cpu_rt}, false, true, F_modifiesRd}, // r5900_pmultw
    {"pdivw", {Operand::cpu_rs, Operand::cpu_rt}, false, true, 0}, // r5900_pdivw
    {"pcpyld", {Operand::cpu_rd, Operand::cpu_rs, Operand::cpu_rt}, false, true, F_modifiesRd}, // r5900_pcpyld
    {"pmaddh", {Operand::cpu_rd, Operand::cpu_rs, Operand::cpu_rt}, false, true, F_modifiesRd}, // r5900_pmaddh
    {"phmadh", {Operand::cpu_rd, Operand::cpu_rs, Operand::cpu_rt}, false, true, F_modifiesRd}, // r5900_phmadh
    {"pand", {Operand::cpu_rd, Operand::cpu_rs, Operand::cpu_rt}, false, true, F_modifiesRd}, // r5900_pand
    {"pxor", {Operand::cpu_rd, Operand::cpu_rs, Operand::cpu_rt}, false, true, F_modifiesRd}, // r5900_pxor
    {"pmsubh", {Operand::cpu_rd, Operand::cpu_rs, Operand::cpu_rt}, false, true, F_modifiesRd}, // r5900_pmsubh
    {"phmsbh", {Operand::cpu_rd, Operand::cpu_rs, Operand::cpu_rt}, false, true, F_modifiesRd}, // r5900_phmsbh
    {"pexeh", {Operand::cpu_rd, Operand::cpu_rt}, false, true, F_modifiesRd}, // r5900_pexeh
    {"prevh", {Operand::cpu_rd, Operand::cpu_rt}, false, true, F_modifiesRd}, // r5900_prevh
    {"pmulth", {Operand::cpu_rd, Operand::cpu_rs, Operand::cpu_rt}, false, true, F_modifiesRd}, // r5900_pmulth
    {"pdivbw", {Operand::cpu_rs, Operand::cpu_rt}, false, true, 0}, // r5900_pdivbw
    {"pexew", {Operand::cpu_rd, Operand::cpu_rt}, false, true, F_modifiesRd}, // r5900_pexew
    {"prot3w", {Operand::cpu_rd, Operand::cpu_rt}, false, true, F_modifiesRd}, // r5900_prot3w
    {"pmadduw", {Operand::cpu_rd, Operand::cpu_rs, Operand::cpu_rt}, false, true, F_modifiesRd}, // r5900_pmadduw
    {"psravw", {Operand::cpu_rd, Operand::cpu_rt, Operand::cpu_rs}, false, true, F_modifiesRd}, // r5900_psravw
    {"pmthi", {Operand::cpu_rs}, false, true, 0}, // r5900_pmthi
    {"pmtlo", {Operand::cpu_rs}, false, true, 0}, // r5900_pmtlo
    {"pinteh", {Operand::cpu_rd, Operand::cpu_rs, Operand::cpu_rt}, false, true, F_modifiesRd}, // r5900_pinteh
    {"pmultuw", {Operand::cpu_rd, Operand::cpu_rs, Operand::cpu_rt}, false, true, F_modifiesRd}, // r5900_pmultuw
    {"pdivuw", {Operand::cpu_rs, Operand::cpu_rt}, false, true, 0}, // r5900_pdivuw
    {"pcpyud", {Operand::cpu_rd, Operand::cpu_rs, Operand::cpu_rt}, false, true, F_modifiesRd}, // r5900_pcpyud
    {"por", {Operand::cpu_rd, Operand::cpu_rs, Operand::cpu_rt}, false, true, F_modifiesRd}, // r5900_por
    {"pnor", {Operand::cpu_rd, Operand::cpu_rs, Operand::cpu_rt}, false, true, F_modifiesRd}, // r5900_pnor
    {"pexch", {Operand::cpu_rd, Operand::cpu_rt}, false, true, F_modifiesRd}, // r5900_pexch
    {"pcpyh", {Operand::cpu_rd, Operand::cpu_rt}, false, true, F_modifiesRd}, // r5900_pcpyh
    {"pexcw", {Operand::cpu_rd, Operand::cpu_rt}, false, true, F_modifiesRd}, // r5900_pexcw
    {"pmfhl.lw", {Operand::cpu_rd}, false, true, F_modifiesRd}, // r5900_pmfhl_lw
    {"pmfhl.uw", {Operand::cpu_rd}, false, true, F_modifiesRd}, // r5900_pmfhl_uw
    {"pmfhl.slw", {Operand::cpu_rd}, false, true, F_modifiesRd}, // r5900_pmfhl_slw
    {"pmfhl.lh", {Operand::cpu_rd}, false, true, F_modifiesRd}, // r5900_pmfhl_lh
    {"pmfhl.sh", {Operand::cpu_rd}, false, true, F_modifiesRd}, // r5900_pmfhl_sh
    {"pmthl.lw", {Operand::cpu_rs}, false, true, 0}, // r5900_pmthl_lw
    {"ei", {Operand::None}, false, true, 0}, // r5900_ei
    {"di", {Operand::None}, false, true, 0}, // r5900_di
    {"c1", {Operand::cpu_copraw}, false, true, 0}, // r5900_c1__sqrt_s
    {"rsqrt.s", {Operand::cpu_fd, Operand::cpu_fs, Operand::cpu_ft}, false, true, F_modifiesFd}, // r5900_rsqrt_s
    {"adda.s", {Operand::cpu_fs, Operand::cpu_ft}, false, true, F_modifiesFs}, // r5900_adda_s
    {"suba.s", {Operand::cpu_fs, Operand::cpu_ft}, false, true, F_modifiesFs}, // r5900_suba_s
    {"mula.s", {Operand::cpu_fs, Operand::cpu_ft}, false, true, F_modifiesFs}, // r5900_mula_s
    {"madd.s", {Operand::cpu_fd, Operand::cpu_fs, Operand::cpu_ft}, false, true, F_modifiesFd}, // r5900_madd_s
    {"msub.s", {Operand::cpu_fd, Operand::cpu_fs, Operand::cpu_ft}, false, true, F_modifiesFd}, // r5900_msub_s
    {"madda.s", {Operand::cpu_fs, Operand::cpu_ft}, false, true, 0}, // r5900_madda_s
    {"msuba.s", {Operand::cpu_fs, Operand::cpu_ft}, false, true, 0}, // r5900_msuba_s
    {"max.s", {Operand::cpu_fd, Operand::cpu_fs, Operand::cpu_ft}, false, true, F_modifiesFd}, // r5900_max_s
    {"min.s", {Operand::cpu_fd, Operand::cpu_fs, Operand::cpu_ft}, false, true, F_modifiesFd}, // r5900_min_s
    {"c.lt.s", {Operand::cpu_fs, Operand::cpu_ft}, false, true, 0}, // r5900_c_lt_s
    {"c.le.s", {Operand::cpu_fs, Operand::cpu_ft}, false, true, 0}, // r5900_c_le_s
    {"qmfc2.ni", {Operand::cpu_rt, Operand::r5900_vfs}, false, true, F_modifiesRt}, // r5900_qmfc2_ni
    {"cfc2.ni", {Operand::cpu_rt, Operand::r5900_vis}, false, true, F_modifiesRt}, // r5900_cfc2_ni
    {"qmtc2.ni", {Operand::cpu_rt, Operand::r5900_vfs}, false, true, 0}, // r5900_qmtc2_ni
    {"ctc2.ni", {Operand::cpu_rt, Operand::r5900_vis}, false, true, 0}, // r5900_ctc2_ni
    {"qmfc2.i", {Operand::cpu_rt, Operand::r5900_vfs}, false, true, F_modifiesRt}, // r5900_qmfc2_i
    {"cfc2.i", {Operand::cpu_rt, Operand::r5900_vis}, false, true, F_modifiesRt}, // r5900_cfc2_i
    {"qmtc2.i", {Operand::cpu_rt, Operand::r5900_vfs}, false, true, 0}, // r5900_qmtc2_i
    {"ctc2.i", {Operand::cpu_rt, Operand::r5900_vis}, false, true, 0}, // r5900_ctc2_i
    {"bc2f", {Operand::cpu_branch_target_label}, false, true, F_isBranch}, // r5900_bc2f
    {"bc2t", {Operand::cpu_branch_target_label}, false, true, F_isBranch}, // r5900_bc2t
    {"bc2fl", {Operand::cpu_branch_target_label}, false, true, F_isBranch}, // r5900_bc2fl
    {"bc2tl", {Operand::cpu_branch_target_label}, false, true, F_isBranch}, // r5900_bc2tl
    {"vaddx", {Operand::r5900_vfdxyzw, Operand::r5900_vfsxyzw, Operand::r5900_vftn}, true, true, 0}, // r5900_vaddx
    {"vaddy", {Operand::r5900_vfdxyzw, Operand::r5900_vfsxyzw, Operand::r5900_vftn}, true, true, 0}, // r5900_vaddy
    {"vaddz", {Operand::r5900_vfdxyzw, Operand::r5900_vfsxyzw, Operand::r5900_vftn}, true, true, 0}, // r5900_vaddz
    {"vaddw", {Operand::r5900_vfdxyzw, Operand::r5900_vfsxyzw, Operand::r5900_vftn}, true, true, 0}, // r5900_vaddw
    {"vsubx", {Operand::r5900_vfdxyzw, Operand::r5900_vfsxyzw, Operand::r5900_vftn}, true, true, 0}, // r5900_vsubx
    {"vsuby", {Operand::r5900_vfdxyzw, Operand::r5900_vfsxyzw, Operand::r5900_vftn}, true, true, 0}, // r5900_vsuby
    {"vsubz", {Operand::r5900_vfdxyzw, Operand::r5900_vfsxyzw, Operand::r5900_vftn}, true, true, 0}, // r5900_vsubz
    {"vsubw", {Operand::r5900_vfdxyzw, Operand::r5900_vfsxyzw, Operand::r5900_vftn}, true, true, 0}, // r5900_vsubw
    {"vmaddx", {Operand::r5900_vfdxyzw, Operand::r5900_vfsxyzw, Operand::r5900_vftn}, true, true, 0}, // r5900_vmaddx
    {"vmaddy", {Operand::r5900_vfdxyzw, Operand::r5900_vfsxyzw, Operand::r5900_vftn}, true, true, 0}, // r5900_vmaddy
    {"vmaddz", {Operand::r5900_vfdxyzw, Operand::r5900_vfsxyzw, Operand::r5900_vftn}, true, true, 0}, // r5900_vmaddz
    {"vmaddw", {Operand::r5900_vfdxyzw, Operand::r5900_vfsxyzw, Operand::r5900_vftn}, true, true, 0}, // r5900_vmaddw
    {"vmsubx", {Operand::r5900_vfdxyzw, Operand::r5900_vfsxyzw, Operand::r5900_vftn}, true, true, 0}, // r5900_vmsubx
    {"vmsuby", {Operand::r5900_vfdxyzw, Operand::r5900_vfsxyzw, Operand::r5900_vftn}, true, true, 0}, // r5900_vmsuby
    {"vmsubz", {Operand::r5900_vfdxyzw, Operand::r5900_vfsxyzw, Operand::r5900_vftn}, true, true, 0}, // r5900_vmsubz
    {"vmsubw", {Operand::r5900_vfdxyzw, Operand::r5900_vfsxyzw, Operand::r5900_vftn}, true, true, 0}, // r5900_vmsubw
    {"vmaxx", {Operand::r5900_vfdxyzw, Operand::r5900_vfsxyzw, Operand::r5900_vftn}, true, true, 0}, // r5900_vmaxx
    {"vmaxy", {Operand::r5900_vfdxyzw, Operand::r5900_vfsxyzw, Operand::r5900_vftn}, true, true, 0}, // r5900_vmaxy
    {"vmaxz", {Operand::r5900_vfdxyzw, Operand::r5900_vfsxyzw, Operand::r5900_vftn}, true, true, 0}, // r5900_vmaxz
    {"vmaxw", {Operand::r5900_vfdxyzw, Operand::r5900_vfsxyzw, Operand::r5900_vftn}, true, true, 0}, // r5900_vmaxw
    {"vminix", {Operand::r5900_vfdxyzw, Operand::r5900_vfsxyzw, Operand::r5900_vftn}, true, true, 0}, // r5900_vminix
    {"vminiy", {Operand::r5900_vfdxyzw, Operand::r5900_vfsxyzw, Operand::r5900_vftn}, true, true, 0}, // r5900_vminiy
    {"vminiz", {Operand::r5900_vfdxyzw, Operand::r5900_vfsxyzw, Operand::r5900_vftn}, true, true, 0}, // r5900_vminiz
    {"vminiw", {Operand::r5900_vfdxyzw, Operand::r5900_vfsxyzw, Operand::r5900_vftn}, true, true, 0}, // r5900_vminiw
    {"vmulx", {Operand::r5900_vfdxyzw, Operand::r5900_vfsxyzw, Operand::r5900_vftn}, true, true, 0}, // r5900_vmulx
    {"vmuly", {Operand::r5900_vfdxyzw, Operand::r5900_vfsxyzw, Operand::r5900_vftn}, true, true, 0}, // r5900_vmuly
    {"vmulz", {Operand::r5900_vfdxyzw, Operand::r5900_vfsxyzw, Operand::r5900_vftn}, true, true, 0}, // r5900_vmulz
    {"vmulw", {Operand::r5900_vfdxyzw, Operand::r5900_vfsxyzw, Operand::r5900_vftn}, true, true, 0}, // r5900_vmulw
    {"vmulq", {Operand::r5900_vfdxyzw, Operand::r5900_vfsxyzw, Operand::r5900_Q}, true, true, 0}, // r5900_vmulq
    {"vmaxi", {Operand::r5900_vfdxyzw, Operand::r5900_vfsxyzw, Operand::r5900_I}, true, true, 0}, // r5900_vmaxi
    {"vmuli", {Operand::r5900_vfdxyzw, Operand::r5900_vfsxyzw, Operand::r5900_I}, true, true, 0}, // r5900_vmuli
    {"vminii", {Operand::r5900_vfdxyzw, Operand::r5900_vfsxyzw, Operand::r5900_I}, true, true, 0}, // r5900_vminii
    {"vaddq", {Operand::r5900_vfdxyzw, Operand::r5900_vfsxyzw, Operand::r5900_Q}, true, true, 0}, // r5900_vaddq
    {"vmaddq", {Operand::r5900_vfdxyzw, Operand::r5900_vfsxyzw, Operand::r5900_Q}, true, true, 0}, // r5900_vmaddq
    {"vaddi", {Operand::r5900_vfdxyzw, Operand::r5900_vfsxyzw, Operand::r5900_I}, true, true, 0}, // r5900_vaddi
    {"vmaddi", {Operand::r5900_vfdxyzw, Operand::r5900_vfsxyzw, Operand::r5900_I}, true, true, 0}, // r5900_vmaddi
    {"vsubq", {Operand::r5900_vfdxyzw, Operand::r5900_vfsxyzw, Operand::r5900_Q}, true, true, 0}, // r5900_vsubq
    {"vmsubq", {Operand::r5900_vfdxyzw, Operand::r5900_vfsxyzw, Operand::r5900_Q}, true, true, 0}, // r5900_vmsubq
    {"vsubi", {Operand::r5900_vfdxyzw, Operand::r5900_vfsxyzw, Operand::r5900_I}, true, true, 0}, // r5900_vsubi
    {"vmsubi", {Operand::r5900_vfdxyzw, Operand::r5900_vfsxyzw, Operand::r5900_I}, true, true, 0}, // r5900_vmsubi
    {"vadd", {Operand::r5900_vfdxyzw, Operand::r5900_vfsxyzw, Operand::r5900_vftxyzw}, true, true, 0}, // r5900_vadd
    {"vmadd", {Operand::r5900_vfdxyzw, Operand::r5900_vfsxyzw, Operand::r5900_vftxyzw}, true, true, 0}, // r5900_vmadd
    {"vmul", {Operand::r5900_vfdxyzw, Operand::r5900_vfsxyzw, Operand::r5900_vftxyzw}, true, true, 0}, // r5900_vmul
    {"vmax", {Operand::r5900_vfdxyzw, Operand::r5900_vfsxyzw, Operand::r5900_vftxyzw}, true, true, 0}, // r5900_vmax
    {"vsub", {Operand::r5900_vfdxyzw, Operand::r5900_vfsxyzw, Operand::r5900_vftxyzw}, true, true, 0}, // r5900_vsub
    {"vmsub", {Operand::r5900_vfdxyzw, Operand::r5900_vfsxyzw, Operand::r5900_vftxyzw}, true, true, 0}, // r5900_vmsub
    {"vopmsub", {Operand::r5900_vfdxyzw, Operand::r5900_vfsxyzw, Operand::r5900_vftxyzw}, true, true, 0}, // r5900_vopmsub
    {"vmini", {Operand::r5900_vfdxyzw, Operand::r5900_vfsxyzw, Operand::r5900_vftxyzw}, true, true, 0}, // r5900_vmini
    {"viadd", {Operand::r5900_vid, Operand::r5900_vis, Operand::r5900_vit}, false, true, 0}, // r5900_viadd
    {"visub", {Operand::r5900_vid, Operand::r5900_vis, Operand::r5900_vit}, false, true, 0}, // r5900_visub
    {"viaddi", {Operand::r5900_vit, Operand::r5900_vis, Operand::r5900_immediate5}, false, true, 0}, // r5900_viaddi
    {"viand", {Operand::r5900_vid, Operand::r5900_vis, Operand::r5900_vit}, false, true, 0}, // r5900_viand
    {"vior", {Operand::r5900_vid, Operand::r5900_vis, Operand::r5900_vit}, false, true, 0}, // r5900_vior
    {"vcallms", {Operand::r5900_immediate15}, false, true, 0}, // r5900_vcallms
    {"vcallmsr", {Operand::r5900_vis}, false, true, 0}, // r5900_vcallmsr
    {"vaddax", {Operand::r5900_ACCxyzw, Operand::r5900_vfsxyzw, Operand::r5900_vftn}, true, true, 0}, // r5900_vaddax
    {"vadday", {Operand::r5900_ACCxyzw, Operand::r5900_vfsxyzw, Operand::r5900_vftn}, true, true, 0}, // r5900_vadday
    {"vaddaz", {Operand::r5900_ACCxyzw, Operand::r5900_vfsxyzw, Operand::r5900_vftn}, true, true, 0}, // r5900_vaddaz
    {"vaddaw", {Operand::r5900_ACCxyzw, Operand::r5900_vfsxyzw, Operand::r5900_vftn}, true, true, 0}, // r5900_vaddaw
    {"vsubax", {Operand::r5900_ACCxyzw, Operand::r5900_vfsxyzw, Operand::r5900_vftn}, true, true, 0}, // r5900_vsubax
    {"vsubay", {Operand::r5900_ACCxyzw, Operand::r5900_vfsxyzw, Operand::r5900_vftn}, true, true, 0}, // r5900_vsubay
    {"vsubaz", {Operand::r5900_ACCxyzw, Operand::r5900_vfsxyzw, Operand::r5900_vftn}, true, true, 0}, // r5900_vsubaz
    {"vsubaw", {Operand::r5900_ACCxyzw, Operand::r5900_vfsxyzw, Operand::r5900_vftn}, true, true, 0}, // r5900_vsubaw
    {"vmaddax", {Operand::r5900_ACCxyzw, Operand::r5900_vfsxyzw, Operand::r5900_vftn}, true, true, 0}, // r5900_vmaddax
    {"vmadday", {Operand::r5900_ACCxyzw, Operand::r5900_vfsxyzw, Operand::r5900_vftn}, true, true, 0}, // r5900_vmadday
    {"vmaddaz", {Operand::r5900_ACCxyzw, Operand::r5900_vfsxyzw, Operand::r5900_vftn}, true, true, 0}, // r5900_vmaddaz
    {"vmaddaw", {Operand::r5900_ACCxyzw, Operand::r5900_vfsxyzw, Operand::r5900_vftn}, true, true, 0}, // r5900_vmaddaw
    {"vmsubax", {Operand::r5900_ACCxyzw, Operand::r5900_vfsxyzw, Operand::r5900_vftn}, true, true, 0}, // r5900_vmsubax
    {"vmsubay", {Operand::r5900_ACCxyzw, Operand::r5900_vfsxyzw, Operand::r5900_vftn}, true, true, 0}, // r5900_vmsubay
    {"vmsubaz", {Operand::r5900_ACCxyzw, Operand::r5900_vfsxyzw, Operand::r5900_vftn}, true, true, 0}, // r5900_vmsubaz
    {"vmsubaw", {Operand::r5900_ACCxyzw, Operand::r5900_vfsxyzw, Operand::r5900_vftn}, true, true, 0}, // r5900_vmsubaw
    {"vitof0", {Operand::r5900_vftxyzw, Operand::r5900_vfsxyzw}, true, true, 0}, // r5900_vitof0
    {"vitof4", {Operand::r5900_vftxyzw, Operand::r5900_vfsxyzw}, true, true, 0}, // r5900_vitof4
    {"vitof12", {Operand::r5900_vftxyzw, Operand::r5900_vfsxyzw}, true, true, 0}, // r5900_vitof12
    {"vitof15", {Operand::r5900_vftxyzw, Operand::r5900_vfsxyzw}, true, true, 0}, // r5900_vitof15
    {"vftoi0", {Operand::r5900_vftxyzw, Operand::r5900_vfsxyzw}, true, true, 0}, // r5900_vftoi0
    {"vftoi4", {Operand::r5900_vftxyzw, Operand::r5900_vfsxyzw}, true, true, 0}, // r5900_vftoi4
    {"vftoi12", {Operand::r5900_vftxyzw, Operand::r5900_vfsxyzw}, true, true, 0}, // r5900_vftoi12
    {"vftoi15", {Operand::r5900_vftxyzw, Operand::r5900_vfsxyzw}, true, true, 0}, // r5900_vftoi15
    {"vmulax", {Operand::r5900_ACCxyzw, Operand::r5900_vfsxyzw, Operand::r5900_vftn}, true, true, 0}, // r5900_vmulax
    {"vmulay", {Operand::r5900_ACCxyzw, Operand::r5900_vfsxyzw, Operand::r5900_vftn}, true, true, 0}, // r5900_vmulay
    {"vmulaz", {Operand::r5900_ACCxyzw, Operand::r5900_vfsxyzw, Operand::r5900_vftn}, true, true, 0}, // r5900_vmulaz
    {"vmulaw", {Operand::r5900_ACCxyzw, Operand::r5900_vfsxyzw, Operand::r5900_vftn}, true, true, 0}, // r5900_vmulaw
    {"vmulaq", {Operand::r5900_ACCxyzw, Operand::r5900_vfsxyzw, Operand::r5900_Q}, true, true, 0}, // r5900_vmulaq
    {"vabs", {Operand::r5900_vftxyzw, Operand::r5900_vfsxyzw}, true, true, 0}, // r5900_vabs
    {"vmulai", {Operand::r5900_ACCxyzw, Operand::r5900_vfsxyzw, Operand::r5900_I}, true, true, 0}, // r5900_vmulai
    {"vclipw", {Operand::r5900_vfsxyzw, Operand::r5900_vftn}, true, true, 0}, // r5900_vclipw
    {"vaddaq", {Operand::r5900_ACCxyzw, Operand::r5900_vfsxyzw, Operand::r5900_Q}, true, true, 0}, // r5900_vaddaq
    {"vmaddaq", {Operand::r5900_ACCxyzw, Operand::r5900_vfsxyzw, Operand::r5900_Q}, true, true, 0}, // r5900_vmaddaq
    {"vaddai", {Operand::r5900_ACCxyzw, Operand::r5900_vfsxyzw, Operand::r5900_I}, true, true, 0}, // r5900_vaddai
    {"vmaddai", {Operand::r5900_ACCxyzw, Operand::r5900_vfsxyzw, Operand::r5900_I}, true, true, 0}, // r5900_vmaddai
    {"vsubaq", {Operand::r5900_ACCxyzw, Operand::r5900_vfsxyzw, Operand::r5900_Q}, true, true, 0}, // r5900_vsubaq
    {"vmsubaq", {Operand::r5900_ACCxyzw, Operand::r5900_vfsxyzw, Operand::r5900_Q}, true, true, 0}, // r5900_vmsubaq
    {"vsubai", {Operand::r5900_ACCxyzw, Operand::r5900_vfsxyzw, Operand::r5900_I}, true, true, 0}, // r5900_vsubai
    {"vmsubai", {Operand::r5900_ACCxyzw, Operand::r5900_vfsxyzw, Operand::r5900_I}, true, true, 0}, // r5900_vmsubai
    {"vadda", {Operand::r5900_ACCxyzw, Operand::r5900_vftxyzw, Operand::r5900_vfsxyzw}, true, true, 0}, // r5900_vadda
    {"vmadda", {Operand::r5900_ACCxyzw, Operand::r5900_vfsxyzw, Operand::r5900_vftxyzw}, true, true, 0}, // r5900_vmadda
    {"vmula", {Operand::r5900_ACCxyzw, Operand::r5900_vfsxyzw, Operand::r5900_vftxyzw}, true, true, 0}, // r5900_vmula
    {"vsuba", {Operand::r5900_ACCxyzw, Operand::r5900_vfsxyzw, Operand::r5900_vftxyzw}, true, true, 0}, // r5900_vsuba
    {"vmsuba", {Operand::r5900_ACCxyzw, Operand::r5900_vftxyzw, Operand::r5900_vfsxyzw}, true, true, 0}, // r5900_vmsuba
    {"vopmula", {Operand::r5900_ACCxyzw, Operand::r5900_vfsxyzw, Operand::r5900_vftxyzw}, true, true, 0}, // r5900_vopmula
    {"vnop", {Operand::None}, false, true, 0}, // r5900_vnop
    {"vmove", {Operand::r5900_vftxyzw, Operand::r5900_vfsxyzw}, true, true, 0}, // r5900_vmove
    {"vmr32", {Operand::r5900_vftxyzw, Operand::r5900_vfsxyzw}, true, true, 0}, // r5900_vmr32
    {"vlqi", {Operand::r5900_vftxyzw, Operand::r5900_vis_postincr}, true, true, 0}, // r5900_vlqi
    {"vsqi", {Operand::r5900_vfsxyzw, Operand::r5900_vit_postincr}, true, true, 0}, // r5900_vsqi
    {"vlqd", {Operand::r5900_vftxyzw, Operand::r5900_vis_predecr}, true, true, 0}, // r5900_vlqd
    {"vsqd", {Operand::r5900_vfsxyzw, Operand::r5900_vit_predecr}, true, true, 0}, // r5900_vsqd
    {"vdiv", {Operand::r5900_Q, Operand::r5900_vfsl, Operand::r5900_vftm}, false, true, 0}, // r5900_vdiv
    {"vsqrt", {Operand::r5900_Q, Operand::r5900_vftm}, false, true, 0}, // r5900_vsqrt
    {"vrsqrt", {Operand::r5900_Q, Operand::r5900_vfsl, Operand::r5900_vftm}, false, true, 0}, // r5900_vrsqrt
    {"vwaitq", {Operand::None}, false, true, 0}, // r5900_vwaitq
    {"vmtir", {Operand::r5900_vit, Operand::r5900_vfsl}, false, true, 0}, // r5900_vmtir
    {"vmfir", {Operand::r5900_vftxyzw, Operand::r5900_vis}, true, true, 0}, // r5900_vmfir
    {"vrnext", {Operand::r5900_vftxyzw, Operand::r5900_R}, true, true, 0}, // r5900_vrnext
    {"vrget", {Operand::r5900_vftxyzw, Operand::r5900_R}, true, true, 0}, // r5900_vrget
    {"vrinit", {Operand::r5900_R, Operand::r5900_vfsl}, false, true, 0}, // r5900_vrinit
    {"vrxor", {Operand::r5900_R, Operand::r5900_vfsl}, false, true, 0}, // r5900_vrxor
    {"vilwr.w", {Operand::r5900_vit, Operand::r5900_vis_parenthesis}, false, true, F_doesLoad}, // r5900_vilwr_w
    {"vilwr.z", {Operand::r5900_vit, Operand::r5900_vis_parenthesis}, false, true, F_doesLoad}, // r5900_vilwr_z
    {"vilwr.y", {Operand::r5900_vit, Operand::r5900_vis_parenthesis}, false, true, F_doesLoad}, // r5900_vilwr_y
    {"vilwr.x", {Operand::r5900_vit, Operand::r5900_vis_parenthesis}, false, true, F_doesLoad}, // r5900_vilwr_x
    {"viswr.w", {Operand::r5900_vit, Operand::r5900_vis_parenthesis}, false, true, F_doesLoad}, // r5900_viswr_w
    {"viswr.z", {Operand::r5900_vit, Operand::r5900_vis_parenthesis}, false, true, F_doesLoad}, // r5900_viswr_z
    {"viswr.y", {Operand::r5900_vit, Operand::r5900_vis_parenthesis}, false, true, F_doesLoad}, // r5900_viswr_y
    {"viswr.x", {Operand::r5900_vit, Operand::r5900_vis_parenthesis}, false, true, F_doesLoad}, // r5900_viswr_x
};

const uint16_t kCpuNormal[64] = {
    kNoId, kNoId, ID_cpu_j, ID_cpu_jal,
    ID_cpu_beq, ID_cpu_bne, ID_cpu_blez, ID_cpu_bgtz,
    ID_cpu_addi, ID_cpu_addiu, ID_cpu_slti, ID_cpu_sltiu,
    ID_cpu_andi, ID_cpu_ori, ID_cpu_xori, ID_cpu_lui,
    kNoId, kNoId, kNoId, kNoId,
    ID_cpu_beql, ID_cpu_bnel, ID_cpu_blezl, ID_cpu_bgtzl,
    ID_cpu_daddi, ID_cpu_daddiu, ID_cpu_ldl, ID_cpu_ldr,
    kNoId, kNoId, kNoId, kNoId,
    ID_cpu_lb, ID_cpu_lh, ID_cpu_lwl, ID_cpu_lw,
    ID_cpu_lbu, ID_cpu_lhu, ID_cpu_lwr, ID_cpu_lwu,
    ID_cpu_sb, ID_cpu_sh, ID_cpu_swl, ID_cpu_sw,
    ID_cpu_sdl, ID_cpu_sdr, ID_cpu_swr, ID_cpu_cache,
    ID_cpu_ll, ID_cpu_lwc1, ID_cpu_lwc2, ID_cpu_pref,
    ID_cpu_lld, ID_cpu_ldc1, ID_cpu_ldc2, ID_cpu_ld,
    ID_cpu_sc, ID_cpu_swc1, ID_cpu_swc2, kNoId,
    ID_cpu_scd, ID_cpu_sdc1, ID_cpu_sdc2, ID_cpu_sd,
};
const uint16_t kCpuSpecial[64] = {
    ID_cpu_sll, kNoId, ID_cpu_srl, ID_cpu_sra,
    ID_cpu_sllv, kNoId, ID_cpu_srlv, ID_cpu_srav,
    ID_cpu_jr, ID_cpu_jalr, ID_cpu_movz, ID_cpu_movn,
    ID_cpu_syscall, ID_cpu_break, kNoId, ID_cpu_sync,
    ID_cpu_mfhi, ID_cpu_mthi, ID_cpu_mflo, ID_cpu_mtlo,
    ID_cpu_dsllv, kNoId, ID_cpu_dsrlv, ID_cpu_dsrav,
    ID_cpu_mult, ID_cpu_multu, ID_cpu_div, ID_cpu_divu,
    ID_cpu_dmult, ID_cpu_dmultu, ID_cpu_ddiv, ID_cpu_ddivu,
    ID_cpu_add, ID_cpu_addu, ID_cpu_sub, ID_cpu_subu,
    ID_cpu_and, ID_cpu_or, ID_cpu_xor, ID_cpu_nor,
    kNoId, kNoId, ID_cpu_slt, ID_cpu_sltu,
    ID_cpu_dadd, ID_cpu_daddu, ID_cpu_dsub, ID_cpu_dsubu,
    ID_cpu_tge, ID_cpu_tgeu, ID_cpu_tlt, ID_cpu_tltu,
    ID_cpu_teq, kNoId, ID_cpu_tne, kNoId,
    ID_cpu_dsll, kNoId, ID_cpu_dsrl, ID_cpu_dsra,
    ID_cpu_dsll32, kNoId, ID_cpu_dsrl32, ID_cpu_dsra32,
};
const uint16_t kCpuRegimm[32] = {
    ID_cpu_bltz, ID_cpu_bgez, ID_cpu_bltzl, ID_cpu_bgezl,
    kNoId, kNoId, kNoId, kNoId,
    ID_cpu_tgei, ID_cpu_tgeiu, ID_cpu_tlti, ID_cpu_tltiu,
    ID_cpu_teqi, kNoId, ID_cpu_tnei, kNoId,
    ID_cpu_bltzal, ID_cpu_bgezal, ID_cpu_bltzall, ID_cpu_bgezall,
    kNoId, kNoId, kNoId, kNoId,
    kNoId, kNoId, kNoId, kNoId,
    kNoId, kNoId, kNoId, kNoId,
};
const uint16_t kCpuCop0[32] = {
    ID_cpu_mfc0, ID_cpu_dmfc0, ID_cpu_cfc0, kNoId,
    ID_cpu_mtc0, ID_cpu_dmtc0, ID_cpu_ctc0, kNoId,
    kNoId, kNoId, kNoId, kNoId,
    kNoId, kNoId, kNoId, kNoId,
    kNoId, kNoId, kNoId, kNoId,
    kNoId, kNoId, kNoId, kNoId,
    kNoId, kNoId, kNoId, kNoId,
    kNoId, kNoId, kNoId, kNoId,
};
const uint16_t kCpuCop0Bc0[32] = {
    ID_cpu_bc0f, ID_cpu_bc0t, ID_cpu_bc0fl, ID_cpu_bc0tl,
    kNoId, kNoId, kNoId, kNoId,
    kNoId, kNoId, kNoId, kNoId,
    kNoId, kNoId, kNoId, kNoId,
    kNoId, kNoId, kNoId, kNoId,
    kNoId, kNoId, kNoId, kNoId,
    kNoId, kNoId, kNoId, kNoId,
    kNoId, kNoId, kNoId, kNoId,
};
const uint16_t kCpuCop0Tlb[64] = {
    kNoId, ID_cpu_tlbr, ID_cpu_tlbwi, kNoId,
    kNoId, kNoId, ID_cpu_tlbwr, kNoId,
    ID_cpu_tlbp, kNoId, kNoId, kNoId,
    kNoId, kNoId, kNoId, kNoId,
    ID_cpu_rfe, kNoId, kNoId, kNoId,
    kNoId, kNoId, kNoId, kNoId,
    ID_cpu_eret, kNoId, kNoId, kNoId,
    kNoId, kNoId, kNoId, kNoId,
    kNoId, kNoId, kNoId, kNoId,
    kNoId, kNoId, kNoId, kNoId,
    kNoId, kNoId, kNoId, kNoId,
    kNoId, kNoId, kNoId, kNoId,
    kNoId, kNoId, kNoId, kNoId,
    kNoId, kNoId, kNoId, kNoId,
    kNoId, kNoId, kNoId, kNoId,
    kNoId, kNoId, kNoId, kNoId,
};
const uint16_t kCpuCop1[32] = {
    ID_cpu_mfc1, ID_cpu_dmfc1, ID_cpu_cfc1, kNoId,
    ID_cpu_mtc1, ID_cpu_dmtc1, ID_cpu_ctc1, kNoId,
    kNoId, kNoId, kNoId, kNoId,
    kNoId, kNoId, kNoId, kNoId,
    kNoId, kNoId, kNoId, kNoId,
    kNoId, kNoId, kNoId, kNoId,
    kNoId, kNoId, kNoId, kNoId,
    kNoId, kNoId, kNoId, kNoId,
};
const uint16_t kCpuCop1Bc1[32] = {
    ID_cpu_bc1f, ID_cpu_bc1t, ID_cpu_bc1fl, ID_cpu_bc1tl,
    kNoId, kNoId, kNoId, kNoId,
    kNoId, kNoId, kNoId, kNoId,
    kNoId, kNoId, kNoId, kNoId,
    kNoId, kNoId, kNoId, kNoId,
    kNoId, kNoId, kNoId, kNoId,
    kNoId, kNoId, kNoId, kNoId,
    kNoId, kNoId, kNoId, kNoId,
};
const uint16_t kCpuCop1FpuS[64] = {
    ID_cpu_add_s, ID_cpu_sub_s, ID_cpu_mul_s, ID_cpu_div_s,
    ID_cpu_sqrt_s, ID_cpu_abs_s, ID_cpu_mov_s, ID_cpu_neg_s,
    ID_cpu_round_l_s, ID_cpu_trunc_l_s, ID_cpu_ceil_l_s, ID_cpu_floor_l_s,
    ID_cpu_round_w_s, ID_cpu_trunc_w_s, ID_cpu_ceil_w_s, ID_cpu_floor_w_s,
    kNoId, kNoId, kNoId, kNoId,
    kNoId, kNoId, kNoId, kNoId,
    kNoId, kNoId, kNoId, kNoId,
    kNoId, kNoId, kNoId, kNoId,
    kNoId, ID_cpu_cvt_d_s, kNoId, kNoId,
    ID_cpu_cvt_w_s, ID_cpu_cvt_l_s, kNoId, kNoId,
    kNoId, kNoId, kNoId, kNoId,
    kNoId, kNoId, kNoId, kNoId,
    ID_cpu_c_f_s, ID_cpu_c_un_s, ID_cpu_c_eq_s, ID_cpu_c_ueq_s,
    ID_cpu_c_olt_s, ID_cpu_c_ult_s, ID_cpu_c_ole_s, ID_cpu_c_ule_s,
    ID_cpu_c_sf_s, ID_cpu_c_ngle_s, ID_cpu_c_seq_s, ID_cpu_c_ngl_s,
    ID_cpu_c_lt_s, ID_cpu_c_nge_s, ID_cpu_c_le_s, ID_cpu_c_ngt_s,
};
const uint16_t kCpuCop1FpuD[64] = {
    ID_cpu_add_d, ID_cpu_sub_d, ID_cpu_mul_d, ID_cpu_div_d,
    ID_cpu_sqrt_d, ID_cpu_abs_d, ID_cpu_mov_d, ID_cpu_neg_d,
    ID_cpu_round_l_d, ID_cpu_trunc_l_d, ID_cpu_ceil_l_d, ID_cpu_floor_l_d,
    ID_cpu_round_w_d, ID_cpu_trunc_w_d, ID_cpu_ceil_w_d, ID_cpu_floor_w_d,
    kNoId, kNoId, kNoId, kNoId,
    kNoId, kNoId, kNoId, kNoId,
    kNoId, kNoId, kNoId, kNoId,
    kNoId, kNoId, kNoId, kNoId,
    ID_cpu_cvt_s_d, kNoId, kNoId, kNoId,
    ID_cpu_cvt_w_d, ID_cpu_cvt_l_d, kNoId, kNoId,
    kNoId, kNoId, kNoId, kNoId,
    kNoId, kNoId, kNoId, kNoId,
    ID_cpu_c_f_d, ID_cpu_c_un_d, ID_cpu_c_eq_d, ID_cpu_c_ueq_d,
    ID_cpu_c_olt_d, ID_cpu_c_ult_d, ID_cpu_c_ole_d, ID_cpu_c_ule_d,
    ID_cpu_c_df_d, ID_cpu_c_ngle_d, ID_cpu_c_seq_d, ID_cpu_c_ngl_d,
    ID_cpu_c_lt_d, ID_cpu_c_nge_d, ID_cpu_c_le_d, ID_cpu_c_ngt_d,
};
const uint16_t kCpuCop1FpuW[64] = {
    kNoId, kNoId, kNoId, kNoId,
    kNoId, kNoId, kNoId, kNoId,
    kNoId, kNoId, kNoId, kNoId,
    kNoId, kNoId, kNoId, kNoId,
    kNoId, kNoId, kNoId, kNoId,
    kNoId, kNoId, kNoId, kNoId,
    kNoId, kNoId, kNoId, kNoId,
    kNoId, kNoId, kNoId, kNoId,
    ID_cpu_cvt_s_w, ID_cpu_cvt_d_w, kNoId, kNoId,
    kNoId, kNoId, kNoId, kNoId,
    kNoId, kNoId, kNoId, kNoId,
    kNoId, kNoId, kNoId, kNoId,
    kNoId, kNoId, kNoId, kNoId,
    kNoId, kNoId, kNoId, kNoId,
    kNoId, kNoId, kNoId, kNoId,
    kNoId, kNoId, kNoId, kNoId,
};
const uint16_t kCpuCop1FpuL[64] = {
    kNoId, kNoId, kNoId, kNoId,
    kNoId, kNoId, kNoId, kNoId,
    kNoId, kNoId, kNoId, kNoId,
    kNoId, kNoId, kNoId, kNoId,
    kNoId, kNoId, kNoId, kNoId,
    kNoId, kNoId, kNoId, kNoId,
    kNoId, kNoId, kNoId, kNoId,
    kNoId, kNoId, kNoId, kNoId,
    ID_cpu_cvt_s_l, ID_cpu_cvt_d_l, kNoId, kNoId,
    kNoId, kNoId, kNoId, kNoId,
    kNoId, kNoId, kNoId, kNoId,
    kNoId, kNoId, kNoId, kNoId,
    kNoId, kNoId, kNoId, kNoId,
    kNoId, kNoId, kNoId, kNoId,
    kNoId, kNoId, kNoId, kNoId,
    kNoId, kNoId, kNoId, kNoId,
};
const uint16_t kCpuCop2[32] = {
    ID_cpu_mfc2, kNoId, ID_cpu_cfc2, kNoId,
    ID_cpu_mtc2, kNoId, ID_cpu_ctc2, kNoId,
    kNoId, kNoId, kNoId, kNoId,
    kNoId, kNoId, kNoId, kNoId,
    kNoId, kNoId, kNoId, kNoId,
    kNoId, kNoId, kNoId, kNoId,
    kNoId, kNoId, kNoId, kNoId,
    kNoId, kNoId, kNoId, kNoId,
};
const uint16_t kR5900Normal[64] = {
    kNoId, kNoId, kNoId, kNoId,
    kNoId, kNoId, kNoId, kNoId,
    kNoId, kNoId, kNoId, kNoId,
    kNoId, kNoId, kNoId, kNoId,
    kNoId, kNoId, kNoId, kNoId,
    kNoId, kNoId, kNoId, kNoId,
    kNoId, kNoId, kNoId, kNoId,
    kNoId, kNoId, ID_r5900_lq, ID_r5900_sq,
    kNoId, kNoId, kNoId, kNoId,
    kNoId, kNoId, kNoId, kNoId,
    kNoId, kNoId, kNoId, kNoId,
    kNoId, kNoId, kNoId, kNoId,
    kNoId, kNoId, kNoId, kNoId,
    kNoId, kNoId, ID_r5900_lqc2, kNoId,
    kNoId, kNoId, kNoId, kNoId,
    kNoId, kNoId, ID_r5900_sqc2, kNoId,
};
const uint16_t kR5900Special[64] = {
    kNoId, kNoId, kNoId, kNoId,
    kNoId, kNoId, kNoId, kNoId,
    kNoId, kNoId, kNoId, kNoId,
    kNoId, kNoId, kNoId, kNoId,
    kNoId, kNoId, kNoId, kNoId,
    kNoId, kNoId, kNoId, kNoId,
    ID_r5900_mult, kNoId, kNoId, kNoId,
    kNoId, kNoId, kNoId, kNoId,
    kNoId, kNoId, kNoId, kNoId,
    kNoId, kNoId, kNoId, kNoId,
    ID_r5900_mfsa, ID_r5900_mtsa, kNoId, kNoId,
    kNoId, kNoId, kNoId, kNoId,
    kNoId, kNoId, kNoId, kNoId,
    kNoId, kNoId, kNoId, kNoId,
    kNoId, kNoId, kNoId, kNoId,
    kNoId, kNoId, kNoId, kNoId,
};
const uint16_t kR5900Regimm[32] = {
    kNoId, kNoId, kNoId, kNoId,
    kNoId, kNoId, kNoId, kNoId,
    kNoId, kNoId, kNoId, kNoId,
    kNoId, kNoId, kNoId, kNoId,
    kNoId, kNoId, kNoId, kNoId,
    kNoId, kNoId, kNoId, kNoId,
    ID_r5900_mtsab, ID_r5900_mtsah, kNoId, kNoId,
    kNoId, kNoId, kNoId, kNoId,
};
const uint16_t kR5900Cop0Tlb[64] = {
    kNoId, kNoId, kNoId, kNoId,
    kNoId, kNoId, kNoId, kNoId,
    kNoId, kNoId, kNoId, kNoId,
    kNoId, kNoId, kNoId, kNoId,
    kNoId, kNoId, kNoId, kNoId,
    kNoId, kNoId, kNoId, kNoId,
    kNoId, kNoId, kNoId, kNoId,
    kNoId, kNoId, kNoId, kNoId,
    kNoId, kNoId, kNoId, kNoId,
    kNoId, kNoId, kNoId, kNoId,
    kNoId, kNoId, kNoId, kNoId,
    kNoId, kNoId, kNoId, kNoId,
    kNoId, kNoId, kNoId, kNoId,
    kNoId, kNoId, kNoId, kNoId,
    ID_r5900_ei, ID_r5900_di, kNoId, kNoId,
    kNoId, kNoId, kNoId, kNoId,
};
const uint16_t kR5900Cop1FpuS[64] = {
    kNoId, kNoId, kNoId, kNoId,
    ID_r5900_c1__sqrt_s, kNoId, kNoId, kNoId,
    kNoId, kNoId, kNoId, kNoId,
    kNoId, kNoId, kNoId, kNoId,
    kNoId, kNoId, kNoId, kNoId,
    kNoId, kNoId, ID_r5900_rsqrt_s, kNoId,
    ID_r5900_adda_s, ID_r5900_suba_s, ID_r5900_mula_s, kNoId,
    ID_r5900_madd_s, ID_r5900_msub_s, ID_r5900_madda_s, ID_r5900_msuba_s,
    kNoId, kNoId, kNoId, kNoId,
    kNoId, kNoId, kNoId, kNoId,
    ID_r5900_max_s, ID_r5900_min_s, kNoId, kNoId,
    kNoId, kNoId, kNoId, kNoId,
    kNoId, kNoId, kNoId, kNoId,
    ID_r5900_c_lt_s, kNoId, ID_r5900_c_le_s, kNoId,
    kNoId, kNoId, kNoId, kNoId,
    kNoId, kNoId, kNoId, kNoId,
};
const uint16_t kR5900Cop2Ni[16] = {
    kNoId, ID_r5900_qmfc2_ni, ID_r5900_cfc2_ni, kNoId,
    kNoId, ID_r5900_qmtc2_ni, ID_r5900_ctc2_ni, kNoId,
    kNoId, kNoId, kNoId, kNoId,
    kNoId, kNoId, kNoId, kNoId,
};
const uint16_t kR5900Cop2I[16] = {
    kNoId, ID_r5900_qmfc2_i, ID_r5900_cfc2_i, kNoId,
    kNoId, ID_r5900_qmtc2_i, ID_r5900_ctc2_i, kNoId,
    kNoId, kNoId, kNoId, kNoId,
    kNoId, kNoId, kNoId, kNoId,
};
const uint16_t kR5900Cop2Bc2[32] = {
    ID_r5900_bc2f, ID_r5900_bc2t, ID_r5900_bc2fl, ID_r5900_bc2tl,
    kNoId, kNoId, kNoId, kNoId,
    kNoId, kNoId, kNoId, kNoId,
    kNoId, kNoId, kNoId, kNoId,
    kNoId, kNoId, kNoId, kNoId,
    kNoId, kNoId, kNoId, kNoId,
    kNoId, kNoId, kNoId, kNoId,
    kNoId, kNoId, kNoId, kNoId,
};
const uint16_t kR5900Cop2Special1[64] = {
    ID_r5900_vaddx, ID_r5900_vaddy, ID_r5900_vaddz, ID_r5900_vaddw,
    ID_r5900_vsubx, ID_r5900_vsuby, ID_r5900_vsubz, ID_r5900_vsubw,
    ID_r5900_vmaddx, ID_r5900_vmaddy, ID_r5900_vmaddz, ID_r5900_vmaddw,
    ID_r5900_vmsubx, ID_r5900_vmsuby, ID_r5900_vmsubz, ID_r5900_vmsubw,
    ID_r5900_vmaxx, ID_r5900_vmaxy, ID_r5900_vmaxz, ID_r5900_vmaxw,
    ID_r5900_vminix, ID_r5900_vminiy, ID_r5900_vminiz, ID_r5900_vminiw,
    ID_r5900_vmulx, ID_r5900_vmuly, ID_r5900_vmulz, ID_r5900_vmulw,
    ID_r5900_vmulq, ID_r5900_vmaxi, ID_r5900_vmuli, ID_r5900_vminii,
    ID_r5900_vaddq, ID_r5900_vmaddq, ID_r5900_vaddi, ID_r5900_vmaddi,
    ID_r5900_vsubq, ID_r5900_vmsubq, ID_r5900_vsubi, ID_r5900_vmsubi,
    ID_r5900_vadd, ID_r5900_vmadd, ID_r5900_vmul, ID_r5900_vmax,
    ID_r5900_vsub, ID_r5900_vmsub, ID_r5900_vopmsub, ID_r5900_vmini,
    ID_r5900_viadd, ID_r5900_visub, ID_r5900_viaddi, kNoId,
    ID_r5900_viand, ID_r5900_vior, kNoId, kNoId,
    ID_r5900_vcallms, ID_r5900_vcallmsr, kNoId, kNoId,
    kNoId, kNoId, kNoId, kNoId,
};
const uint16_t kR5900Cop2Special2[128] = {
    ID_r5900_vaddax, ID_r5900_vadday, ID_r5900_vaddaz, ID_r5900_vaddaw,
    ID_r5900_vsubax, ID_r5900_vsubay, ID_r5900_vsubaz, ID_r5900_vsubaw,
    ID_r5900_vmaddax, ID_r5900_vmadday, ID_r5900_vmaddaz, ID_r5900_vmaddaw,
    ID_r5900_vmsubax, ID_r5900_vmsubay, ID_r5900_vmsubaz, ID_r5900_vmsubaw,
    ID_r5900_vitof0, ID_r5900_vitof4, ID_r5900_vitof12, ID_r5900_vitof15,
    ID_r5900_vftoi0, ID_r5900_vftoi4, ID_r5900_vftoi12, ID_r5900_vftoi15,
    ID_r5900_vmulax, ID_r5900_vmulay, ID_r5900_vmulaz, ID_r5900_vmulaw,
    ID_r5900_vmulaq, ID_r5900_vabs, ID_r5900_vmulai, ID_r5900_vclipw,
    ID_r5900_vaddaq, ID_r5900_vmaddaq, ID_r5900_vaddai, ID_r5900_vmaddai,
    ID_r5900_vsubaq, ID_r5900_vmsubaq, ID_r5900_vsubai, ID_r5900_vmsubai,
    ID_r5900_vadda, ID_r5900_vmadda, ID_r5900_vmula, kNoId,
    ID_r5900_vsuba, ID_r5900_vmsuba, ID_r5900_vopmula, ID_r5900_vnop,
    ID_r5900_vmove, ID_r5900_vmr32, kNoId, kNoId,
    ID_r5900_vlqi, ID_r5900_vsqi, ID_r5900_vlqd, ID_r5900_vsqd,
    ID_r5900_vdiv, ID_r5900_vsqrt, ID_r5900_vrsqrt, ID_r5900_vwaitq,
    ID_r5900_vmtir, ID_r5900_vmfir, kNoId, kNoId,
    ID_r5900_vrnext, ID_r5900_vrget, ID_r5900_vrinit, ID_r5900_vrxor,
    kNoId, kNoId, kNoId, kNoId,
    kNoId, kNoId, kNoId, kNoId,
    kNoId, kNoId, kNoId, kNoId,
    kNoId, kNoId, kNoId, kNoId,
    kNoId, kNoId, kNoId, kNoId,
    kNoId, kNoId, kNoId, kNoId,
    kNoId, kNoId, kNoId, kNoId,
    kNoId, kNoId, kNoId, kNoId,
    kNoId, kNoId, kNoId, kNoId,
    kNoId, kNoId, kNoId, kNoId,
    kNoId, kNoId, kNoId, kNoId,
    kNoId, kNoId, kNoId, kNoId,
    kNoId, kNoId, kNoId, kNoId,
    kNoId, kNoId, kNoId, kNoId,
    kNoId, kNoId, kNoId, kNoId,
};
const uint16_t kR5900Cop2Viwr[64] = {
    kNoId, kNoId, kNoId, kNoId,
    kNoId, kNoId, ID_r5900_vilwr_w, ID_r5900_viswr_w,
    kNoId, kNoId, ID_r5900_vilwr_z, ID_r5900_viswr_z,
    kNoId, kNoId, kNoId, kNoId,
    kNoId, kNoId, ID_r5900_vilwr_y, ID_r5900_viswr_y,
    kNoId, kNoId, kNoId, kNoId,
    kNoId, kNoId, kNoId, kNoId,
    kNoId, kNoId, kNoId, kNoId,
    kNoId, kNoId, ID_r5900_vilwr_x, ID_r5900_viswr_x,
    kNoId, kNoId, kNoId, kNoId,
    kNoId, kNoId, kNoId, kNoId,
    kNoId, kNoId, kNoId, kNoId,
    kNoId, kNoId, kNoId, kNoId,
    kNoId, kNoId, kNoId, kNoId,
    kNoId, kNoId, kNoId, kNoId,
    kNoId, kNoId, kNoId, kNoId,
};
const uint16_t kR5900Mmi[64] = {
    ID_r5900_madd, ID_r5900_maddu, kNoId, kNoId,
    ID_r5900_plzcw, kNoId, kNoId, kNoId,
    kNoId, kNoId, kNoId, kNoId,
    kNoId, kNoId, kNoId, kNoId,
    ID_r5900_mfhi1, ID_r5900_mthi1, ID_r5900_mflo1, ID_r5900_mtlo1,
    kNoId, kNoId, kNoId, kNoId,
    ID_r5900_mult1, ID_r5900_multu1, ID_r5900_div1, ID_r5900_divu1,
    kNoId, kNoId, kNoId, kNoId,
    ID_r5900_madd1, ID_r5900_maddu1, kNoId, kNoId,
    kNoId, kNoId, kNoId, kNoId,
    kNoId, kNoId, kNoId, kNoId,
    kNoId, kNoId, kNoId, kNoId,
    kNoId, kNoId, kNoId, kNoId,
    ID_r5900_psllh, kNoId, ID_r5900_psrlh, ID_r5900_psrah,
    kNoId, kNoId, kNoId, kNoId,
    ID_r5900_psllw, kNoId, ID_r5900_psrlw, ID_r5900_psraw,
};
const uint16_t kR5900Mmi0[32] = {
    ID_r5900_paddw, ID_r5900_psubw, ID_r5900_pcgtw, ID_r5900_pmaxw,
    ID_r5900_paddh, ID_r5900_psubh, ID_r5900_pcgth, ID_r5900_pmaxh,
    ID_r5900_paddb, ID_r5900_psubb, ID_r5900_pcgtb, kNoId,
    kNoId, kNoId, kNoId, kNoId,
    ID_r5900_paddsw, ID_r5900_psubsw, ID_r5900_pextlw, ID_r5900_ppacw,
    ID_r5900_paddsh, ID_r5900_psubsh, ID_r5900_pextlh, ID_r5900_ppach,
    ID_r5900_paddsb, ID_r5900_psubsb, ID_r5900_pextlb, ID_r5900_ppacb,
    kNoId, kNoId, ID_r5900_pext5, ID_r5900_ppac5,
};
const uint16_t kR5900Mmi1[32] = {
    kNoId, ID_r5900_pabsw, ID_r5900_pceqw, ID_r5900_pminw,
    ID_r5900_padsbh, ID_r5900_pabsh, ID_r5900_pceqh, ID_r5900_pminh,
    kNoId, kNoId, ID_r5900_pceqb, kNoId,
    kNoId, kNoId, kNoId, kNoId,
    ID_r5900_padduw, ID_r5900_psubuw, ID_r5900_pextuw, kNoId,
    ID_r5900_padduh, ID_r5900_psubuh, ID_r5900_pextuh, kNoId,
    ID_r5900_paddub, ID_r5900_psubub, ID_r5900_pextub, ID_r5900_qfsrv,
    kNoId, kNoId, kNoId, kNoId,
};
const uint16_t kR5900Mmi2[32] = {
    ID_r5900_pmaddw, kNoId, ID_r5900_psllvw, ID_r5900_psrlvw,
    ID_r5900_pmsubw, kNoId, kNoId, kNoId,
    ID_r5900_pmfhi, ID_r5900_pmflo, ID_r5900_pinth, kNoId,
    ID_r5900_pmultw, ID_r5900_pdivw, ID_r5900_pcpyld, kNoId,
    ID_r5900_pmaddh, ID_r5900_phmadh, ID_r5900_pand, ID_r5900_pxor,
    ID_r5900_pmsubh, ID_r5900_phmsbh, kNoId, kNoId,
    kNoId, kNoId, ID_r5900_pexeh, ID_r5900_prevh,
    ID_r5900_pmulth, ID_r5900_pdivbw, ID_r5900_pexew, ID_r5900_prot3w,
};
const uint16_t kR5900Mmi3[32] = {
    ID_r5900_pmadduw, kNoId, kNoId, ID_r5900_psravw,
    kNoId, kNoId, kNoId, kNoId,
    ID_r5900_pmthi, ID_r5900_pmtlo, ID_r5900_pinteh, kNoId,
    ID_r5900_pmultuw, ID_r5900_pdivuw, ID_r5900_pcpyud, kNoId,
    kNoId, kNoId, ID_r5900_por, ID_r5900_pnor,
    kNoId, kNoId, kNoId, kNoId,
    kNoId, kNoId, ID_r5900_pexch, ID_r5900_pcpyh,
    kNoId, kNoId, ID_r5900_pexcw, kNoId,
};
const uint16_t kR5900MmiPmfhl[32] = {
    ID_r5900_pmfhl_lw, ID_r5900_pmfhl_uw, ID_r5900_pmfhl_slw, ID_r5900_pmfhl_lh,
    ID_r5900_pmfhl_sh, kNoId, kNoId, kNoId,
    kNoId, kNoId, kNoId, kNoId,
    kNoId, kNoId, kNoId, kNoId,
    kNoId, kNoId, kNoId, kNoId,
    kNoId, kNoId, kNoId, kNoId,
    kNoId, kNoId, kNoId, kNoId,
    kNoId, kNoId, kNoId, kNoId,
};
const uint16_t kR5900MmiPmthl[32] = {
    ID_r5900_pmthl_lw, kNoId, kNoId, kNoId,
    kNoId, kNoId, kNoId, kNoId,
    kNoId, kNoId, kNoId, kNoId,
    kNoId, kNoId, kNoId, kNoId,
    kNoId, kNoId, kNoId, kNoId,
    kNoId, kNoId, kNoId, kNoId,
    kNoId, kNoId, kNoId, kNoId,
    kNoId, kNoId, kNoId, kNoId,
};

const char *const kGprNames[32] = {
    "$zero", "$at", "$v0", "$v1", "$a0", "$a1", "$a2", "$a3",
    "$t0", "$t1", "$t2", "$t3", "$t4", "$t5", "$t6", "$t7",
    "$s0", "$s1", "$s2", "$s3", "$s4", "$s5", "$s6", "$s7",
    "$t8", "$t9", "$k0", "$k1", "$gp", "$sp", "$fp", "$ra",
};
const char *const kCop0Names[32] = {
    "Index", "Random", "EntryLo0", "EntryLo1", "Context", "PageMask", "Wired", "Reserved07",
    "BadVaddr", "Count", "EntryHi", "Compare", "Status", "Cause", "EPC", "PRevID",
    "Config", "LLAddr", "WatchLo", "WatchHi", "XContext", "Reserved21", "Reserved22", "Reserved23",
    "Reserved24", "Reserved25", "PErr", "CacheErr", "TagLo", "TagHi", "ErrorEPC", "Reserved31",
};
const char *const kCop1Names[32] = {
    "$f0", "$f1", "$f2", "$f3", "$f4", "$f5", "$f6", "$f7",
    "$f8", "$f9", "$f10", "$f11", "$f12", "$f13", "$f14", "$f15",
    "$f16", "$f17", "$f18", "$f19", "$f20", "$f21", "$f22", "$f23",
    "$f24", "$f25", "$f26", "$f27", "$f28", "$f29", "$f30", "$f31",
};
const char *const kCop1ControlNames[32] = {
    "$0", "$1", "$2", "$3", "$4", "$5", "$6", "$7",
    "$8", "$9", "$10", "$11", "$12", "$13", "$14", "$15",
    "$16", "$17", "$18", "$19", "$20", "$21", "$22", "$23",
    "$24", "$25", "$26", "$27", "$28", "$29", "$30", "$FpcCsr",
};
const char *const kCop2Names[32] = {
    "$0", "$1", "$2", "$3", "$4", "$5", "$6", "$7",
    "$8", "$9", "$10", "$11", "$12", "$13", "$14", "$15",
    "$16", "$17", "$18", "$19", "$20", "$21", "$22", "$23",
    "$24", "$25", "$26", "$27", "$28", "$29", "$30", "$31",
};
const char *const kVfNames[32] = {
    "$vf0", "$vf1", "$vf2", "$vf3", "$vf4", "$vf5", "$vf6", "$vf7",
    "$vf8", "$vf9", "$vf10", "$vf11", "$vf12", "$vf13", "$vf14", "$vf15",
    "$vf16", "$vf17", "$vf18", "$vf19", "$vf20", "$vf21", "$vf22", "$vf23",
    "$vf24", "$vf25", "$vf26", "$vf27", "$vf28", "$vf29", "$vf30", "$vf31",
};
const char *const kViNames[32] = {
    "$vi0", "$vi1", "$vi2", "$vi3", "$vi4", "$vi5", "$vi6", "$vi7",
    "$vi8", "$vi9", "$vi10", "$vi11", "$vi12", "$vi13", "$vi14", "$vi15",
    "$vi16", "$vi17", "$vi18", "$vi19", "$vi20", "$vi21", "$vi22", "$vi23",
    "$vi24", "$vi25", "$vi26", "$vi27", "$vi28", "$vi29", "$vi30", "$vi31",
};
