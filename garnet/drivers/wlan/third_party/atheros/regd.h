/*
 * Copyright (c) 2008-2009 Atheros Communications Inc.
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#ifndef REGD_H
#define REGD_H

#include <linux/nl80211.h>
#include <net/cfg80211.h>

#include "ath.h"

enum ctl_group {
    CTL_FCC = 0x10,
    CTL_MKK = 0x40,
    CTL_ETSI = 0x30,
};

// clang-format off
#define NO_CTL                  0xff
#define SD_NO_CTL               0xE0
#define NO_CTL                  0xff
#define CTL_11A                 0
#define CTL_11B                 1
#define CTL_11G                 2
#define CTL_2GHT20              5
#define CTL_5GHT20              6
#define CTL_2GHT40              7
#define CTL_5GHT40              8

#define CTRY_DEBUG              0x1ff
#define CTRY_DEFAULT            0

#define COUNTRY_ERD_FLAG        0x8000
#define WORLDWIDE_ROAMING_FLAG  0x4000

#define MULTI_DOMAIN_MASK       0xFF00

#define WORLD_SKU_MASK          0x00F0
#define WORLD_SKU_PREFIX        0x0060

#define CHANNEL_HALF_BW         10
#define CHANNEL_QUARTER_BW      5
// clang-format on

struct country_code_to_enum_rd {
    uint16_t countryCode;
    uint16_t regDmnEnum;
    const char* isoName;
};

enum CountryCode {
    CTRY_ALBANIA = 8,
    CTRY_ALGERIA = 12,
    CTRY_ARGENTINA = 32,
    CTRY_ARMENIA = 51,
    CTRY_ARUBA = 533,
    CTRY_AUSTRALIA = 36,
    CTRY_AUSTRIA = 40,
    CTRY_AZERBAIJAN = 31,
    CTRY_BAHRAIN = 48,
    CTRY_BANGLADESH = 50,
    CTRY_BARBADOS = 52,
    CTRY_BELARUS = 112,
    CTRY_BELGIUM = 56,
    CTRY_BELIZE = 84,
    CTRY_BOLIVIA = 68,
    CTRY_BOSNIA_HERZ = 70,
    CTRY_BRAZIL = 76,
    CTRY_BRUNEI_DARUSSALAM = 96,
    CTRY_BULGARIA = 100,
    CTRY_CAMBODIA = 116,
    CTRY_CANADA = 124,
    CTRY_CHILE = 152,
    CTRY_CHINA = 156,
    CTRY_COLOMBIA = 170,
    CTRY_COSTA_RICA = 188,
    CTRY_CROATIA = 191,
    CTRY_CYPRUS = 196,
    CTRY_CZECH = 203,
    CTRY_DENMARK = 208,
    CTRY_DOMINICAN_REPUBLIC = 214,
    CTRY_ECUADOR = 218,
    CTRY_EGYPT = 818,
    CTRY_EL_SALVADOR = 222,
    CTRY_ESTONIA = 233,
    CTRY_FAEROE_ISLANDS = 234,
    CTRY_FINLAND = 246,
    CTRY_FRANCE = 250,
    CTRY_GEORGIA = 268,
    CTRY_GERMANY = 276,
    CTRY_GREECE = 300,
    CTRY_GREENLAND = 304,
    CTRY_GRENADA = 308,
    CTRY_GUAM = 316,
    CTRY_GUATEMALA = 320,
    CTRY_HAITI = 332,
    CTRY_HONDURAS = 340,
    CTRY_HONG_KONG = 344,
    CTRY_HUNGARY = 348,
    CTRY_ICELAND = 352,
    CTRY_INDIA = 356,
    CTRY_INDONESIA = 360,
    CTRY_IRAN = 364,
    CTRY_IRAQ = 368,
    CTRY_IRELAND = 372,
    CTRY_ISRAEL = 376,
    CTRY_ITALY = 380,
    CTRY_JAMAICA = 388,
    CTRY_JAPAN = 392,
    CTRY_JORDAN = 400,
    CTRY_KAZAKHSTAN = 398,
    CTRY_KENYA = 404,
    CTRY_KOREA_NORTH = 408,
    CTRY_KOREA_ROC = 410,
    CTRY_KOREA_ROC2 = 411,
    CTRY_KOREA_ROC3 = 412,
    CTRY_KUWAIT = 414,
    CTRY_LATVIA = 428,
    CTRY_LEBANON = 422,
    CTRY_LIBYA = 434,
    CTRY_LIECHTENSTEIN = 438,
    CTRY_LITHUANIA = 440,
    CTRY_LUXEMBOURG = 442,
    CTRY_MACAU = 446,
    CTRY_MACEDONIA = 807,
    CTRY_MALAYSIA = 458,
    CTRY_MALTA = 470,
    CTRY_MEXICO = 484,
    CTRY_MONACO = 492,
    CTRY_MOROCCO = 504,
    CTRY_NEPAL = 524,
    CTRY_NETHERLANDS = 528,
    CTRY_NETHERLANDS_ANTILLES = 530,
    CTRY_NEW_ZEALAND = 554,
    CTRY_NICARAGUA = 558,
    CTRY_NORWAY = 578,
    CTRY_OMAN = 512,
    CTRY_PAKISTAN = 586,
    CTRY_PANAMA = 591,
    CTRY_PAPUA_NEW_GUINEA = 598,
    CTRY_PARAGUAY = 600,
    CTRY_PERU = 604,
    CTRY_PHILIPPINES = 608,
    CTRY_POLAND = 616,
    CTRY_PORTUGAL = 620,
    CTRY_PUERTO_RICO = 630,
    CTRY_QATAR = 634,
    CTRY_ROMANIA = 642,
    CTRY_RUSSIA = 643,
    CTRY_SAUDI_ARABIA = 682,
    CTRY_SERBIA_MONTENEGRO = 891,
    CTRY_SINGAPORE = 702,
    CTRY_SLOVAKIA = 703,
    CTRY_SLOVENIA = 705,
    CTRY_SOUTH_AFRICA = 710,
    CTRY_SPAIN = 724,
    CTRY_SRI_LANKA = 144,
    CTRY_SWEDEN = 752,
    CTRY_SWITZERLAND = 756,
    CTRY_SYRIA = 760,
    CTRY_TAIWAN = 158,
    CTRY_THAILAND = 764,
    CTRY_TRINIDAD_Y_TOBAGO = 780,
    CTRY_TUNISIA = 788,
    CTRY_TURKEY = 792,
    CTRY_UAE = 784,
    CTRY_UKRAINE = 804,
    CTRY_UNITED_KINGDOM = 826,
    CTRY_UNITED_STATES = 840,
    CTRY_UNITED_STATES_FCC49 = 842,
    CTRY_URUGUAY = 858,
    CTRY_UZBEKISTAN = 860,
    CTRY_VENEZUELA = 862,
    CTRY_VIET_NAM = 704,
    CTRY_YEMEN = 887,
    CTRY_ZIMBABWE = 716,
    CTRY_JAPAN1 = 393,
    CTRY_JAPAN2 = 394,
    CTRY_JAPAN3 = 395,
    CTRY_JAPAN4 = 396,
    CTRY_JAPAN5 = 397,
    CTRY_JAPAN6 = 4006,
    CTRY_JAPAN7 = 4007,
    CTRY_JAPAN8 = 4008,
    CTRY_JAPAN9 = 4009,
    CTRY_JAPAN10 = 4010,
    CTRY_JAPAN11 = 4011,
    CTRY_JAPAN12 = 4012,
    CTRY_JAPAN13 = 4013,
    CTRY_JAPAN14 = 4014,
    CTRY_JAPAN15 = 4015,
    CTRY_JAPAN16 = 4016,
    CTRY_JAPAN17 = 4017,
    CTRY_JAPAN18 = 4018,
    CTRY_JAPAN19 = 4019,
    CTRY_JAPAN20 = 4020,
    CTRY_JAPAN21 = 4021,
    CTRY_JAPAN22 = 4022,
    CTRY_JAPAN23 = 4023,
    CTRY_JAPAN24 = 4024,
    CTRY_JAPAN25 = 4025,
    CTRY_JAPAN26 = 4026,
    CTRY_JAPAN27 = 4027,
    CTRY_JAPAN28 = 4028,
    CTRY_JAPAN29 = 4029,
    CTRY_JAPAN30 = 4030,
    CTRY_JAPAN31 = 4031,
    CTRY_JAPAN32 = 4032,
    CTRY_JAPAN33 = 4033,
    CTRY_JAPAN34 = 4034,
    CTRY_JAPAN35 = 4035,
    CTRY_JAPAN36 = 4036,
    CTRY_JAPAN37 = 4037,
    CTRY_JAPAN38 = 4038,
    CTRY_JAPAN39 = 4039,
    CTRY_JAPAN40 = 4040,
    CTRY_JAPAN41 = 4041,
    CTRY_JAPAN42 = 4042,
    CTRY_JAPAN43 = 4043,
    CTRY_JAPAN44 = 4044,
    CTRY_JAPAN45 = 4045,
    CTRY_JAPAN46 = 4046,
    CTRY_JAPAN47 = 4047,
    CTRY_JAPAN48 = 4048,
    CTRY_JAPAN49 = 4049,
    CTRY_JAPAN50 = 4050,
    CTRY_JAPAN51 = 4051,
    CTRY_JAPAN52 = 4052,
    CTRY_JAPAN53 = 4053,
    CTRY_JAPAN54 = 4054,
    CTRY_JAPAN55 = 4055,
    CTRY_JAPAN56 = 4056,
    CTRY_JAPAN57 = 4057,
    CTRY_JAPAN58 = 4058,
    CTRY_JAPAN59 = 4059,
    CTRY_AUSTRALIA2 = 5000,
    CTRY_CANADA2 = 5001,
    CTRY_BELGIUM2 = 5002
};

bool ath_is_world_regd(struct ath_regulatory* reg);
bool ath_is_49ghz_allowed(uint16_t redomain);
uint16_t ath_regd_find_country_by_name(char* alpha2);
int ath_regd_init(struct ath_regulatory* reg, struct wiphy* wiphy,
                  void (*reg_notifier)(struct wiphy* wiphy, struct regulatory_request* request));
uint32_t ath_regd_get_band_ctl(struct ath_regulatory* reg, enum nl80211_band band);
void ath_reg_notifier_apply(struct wiphy* wiphy, struct regulatory_request* request,
                            struct ath_regulatory* reg);

#endif
