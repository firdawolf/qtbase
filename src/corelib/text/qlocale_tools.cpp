/****************************************************************************
**
** Copyright (C) 2016 The Qt Company Ltd.
** Copyright (C) 2016 Intel Corporation.
** Contact: https://www.qt.io/licensing/
**
** This file is part of the QtCore module of the Qt Toolkit.
**
** $QT_BEGIN_LICENSE:LGPL$
** Commercial License Usage
** Licensees holding valid commercial Qt licenses may use this file in
** accordance with the commercial license agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and The Qt Company. For licensing terms
** and conditions see https://www.qt.io/terms-conditions. For further
** information use the contact form at https://www.qt.io/contact-us.
**
** GNU Lesser General Public License Usage
** Alternatively, this file may be used under the terms of the GNU Lesser
** General Public License version 3 as published by the Free Software
** Foundation and appearing in the file LICENSE.LGPL3 included in the
** packaging of this file. Please review the following information to
** ensure the GNU Lesser General Public License version 3 requirements
** will be met: https://www.gnu.org/licenses/lgpl-3.0.html.
**
** GNU General Public License Usage
** Alternatively, this file may be used under the terms of the GNU
** General Public License version 2.0 or (at your option) the GNU General
** Public license version 3 or any later version approved by the KDE Free
** Qt Foundation. The licenses are as published by the Free Software
** Foundation and appearing in the file LICENSE.GPL2 and LICENSE.GPL3
** included in the packaging of this file. Please review the following
** information to ensure the GNU General Public License requirements will
** be met: https://www.gnu.org/licenses/gpl-2.0.html and
** https://www.gnu.org/licenses/gpl-3.0.html.
**
** $QT_END_LICENSE$
**
****************************************************************************/

#include "qlocale_tools_p.h"
#include "qdoublescanprint_p.h"
#include "qlocale_p.h"
#include "qstring.h"

#include <private/qnumeric_p.h>

#include <ctype.h>
#include <errno.h>
#include <float.h>
#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <time.h>

#if defined(Q_OS_LINUX) && !defined(__UCLIBC__)
#    include <fenv.h>
#endif

// Sizes as defined by the ISO C99 standard - fallback
#ifndef LLONG_MAX
#   define LLONG_MAX Q_INT64_C(0x7fffffffffffffff)
#endif
#ifndef LLONG_MIN
#   define LLONG_MIN (-LLONG_MAX - Q_INT64_C(1))
#endif
#ifndef ULLONG_MAX
#   define ULLONG_MAX Q_UINT64_C(0xffffffffffffffff)
#endif

QT_BEGIN_NAMESPACE

QT_WARNING_PUSH
    /* "unary minus operator applied to unsigned type, result still unsigned" */
QT_WARNING_DISABLE_MSVC(4146)
#include "../../3rdparty/freebsd/strtoull.c"
#include "../../3rdparty/freebsd/strtoll.c"
QT_WARNING_POP

QT_CLOCALE_HOLDER

void qt_doubleToAscii(double d, QLocaleData::DoubleForm form, int precision, char *buf, int bufSize,
                      bool &sign, int &length, int &decpt)
{
    if (bufSize == 0) {
        decpt = 0;
        sign = d < 0;
        length = 0;
        return;
    }

    // Detect special numbers (nan, +/-inf)
    // We cannot use the high-level API of libdouble-conversion as we need to
    // apply locale-specific formatting, such as decimal points, grouping
    // separators, etc. Because of this, we have to check for infinity and NaN
    // before calling DoubleToAscii.
    if (qt_is_inf(d)) {
        sign = d < 0;
        if (bufSize >= 3) {
            buf[0] = 'i';
            buf[1] = 'n';
            buf[2] = 'f';
            length = 3;
        } else {
            length = 0;
        }
        return;
    } else if (qt_is_nan(d)) {
        if (bufSize >= 3) {
            buf[0] = 'n';
            buf[1] = 'a';
            buf[2] = 'n';
            length = 3;
        } else {
            length = 0;
        }
        return;
    }

    if (form == QLocaleData::DFSignificantDigits && precision == 0)
        precision = 1; // 0 significant digits is silently converted to 1

#if !defined(QT_NO_DOUBLECONVERSION) && !defined(QT_BOOTSTRAPPED)
    // one digit before the decimal dot, counts as significant digit for DoubleToStringConverter
    if (form == QLocaleData::DFExponent && precision >= 0)
        ++precision;

    double_conversion::DoubleToStringConverter::DtoaMode mode;
    if (precision == QLocale::FloatingPointShortest) {
        mode = double_conversion::DoubleToStringConverter::SHORTEST;
    } else if (form == QLocaleData::DFSignificantDigits || form == QLocaleData::DFExponent) {
        mode = double_conversion::DoubleToStringConverter::PRECISION;
    } else {
        mode = double_conversion::DoubleToStringConverter::FIXED;
    }
    double_conversion::DoubleToStringConverter::DoubleToAscii(d, mode, precision, buf, bufSize,
                                                              &sign, &length, &decpt);
#else // QT_NO_DOUBLECONVERSION || QT_BOOTSTRAPPED

    // Cut the precision at 999, to fit it into the format string. We can't get more than 17
    // significant digits, so anything after that is mostly noise. You do get closer to the "middle"
    // of the range covered by the given double with more digits, so to a degree it does make sense
    // to honor higher precisions. We define that at more than 999 digits that is not the case.
    if (precision > 999)
        precision = 999;
    else if (precision == QLocale::FloatingPointShortest)
        precision = std::numeric_limits<double>::max_digits10; // snprintf lacks "shortest" mode

    if (isZero(d)) {
        // Negative zero is expected as simple "0", not "-0". We cannot do d < 0, though.
        sign = false;
        buf[0] = '0';
        length = 1;
        decpt = 1;
        return;
    } else if (d < 0) {
        sign = true;
        d = -d;
    } else {
        sign = false;
    }

    const int formatLength = 7; // '%', '.', 3 digits precision, 'f', '\0'
    char format[formatLength];
    format[formatLength - 1] = '\0';
    format[0] = '%';
    format[1] = '.';
    format[2] = char((precision / 100) % 10) + '0';
    format[3] = char((precision / 10) % 10)  + '0';
    format[4] = char(precision % 10)  + '0';
    int extraChars;
    switch (form) {
    case QLocaleData::DFDecimal:
        format[formatLength - 2] = 'f';
        // <anything> '.' <precision> '\0'
        extraChars = wholePartSpace(d) + 2;
        break;
    case QLocaleData::DFExponent:
        format[formatLength - 2] = 'e';
        // '.', 'e', '-', <exponent> '\0'
        extraChars = 7;
        break;
    case QLocaleData::DFSignificantDigits:
        format[formatLength - 2] = 'g';

        // either the same as in the 'e' case, or '.' and '\0'
        // precision covers part before '.'
        extraChars = 7;
        break;
    default:
        Q_UNREACHABLE();
    }

    QVarLengthArray<char> target(precision + extraChars);

    length = qDoubleSnprintf(target.data(), target.size(), QT_CLOCALE, format, d);
    int firstSignificant = 0;
    int decptInTarget = length;

    // Find the first significant digit (not 0), and note any '.' we encounter.
    // There is no '-' at the front of target because we made sure d > 0 above.
    while (firstSignificant < length) {
        if (target[firstSignificant] == '.')
            decptInTarget = firstSignificant;
        else if (target[firstSignificant] != '0')
            break;
        ++firstSignificant;
    }

    // If no '.' found so far, search the rest of the target buffer for it.
    if (decptInTarget == length)
        decptInTarget = std::find(target.data() + firstSignificant, target.data() + length, '.') -
                target.data();

    int eSign = length;
    if (form != QLocaleData::DFDecimal) {
        // In 'e' or 'g' form, look for the 'e'.
        eSign = std::find(target.data() + firstSignificant, target.data() + length, 'e') -
                target.data();

        if (eSign < length) {
            // If 'e' is found, the final decimal point is determined by the number after 'e'.
            // Mind that the final decimal point, decpt, is the offset of the decimal point from the
            // start of the resulting string in buf. It may be negative or larger than bufSize, in
            // which case the missing digits are zeroes. In the 'e' case decptInTarget is always 1,
            // as variants of snprintf always generate numbers with one digit before the '.' then.
            // This is why the final decimal point is offset by 1, relative to the number after 'e'.
            bool ok;
            const char *endptr;
            decpt = qstrtoll(target.data() + eSign + 1, &endptr, 10, &ok) + 1;
            Q_ASSERT(ok);
            Q_ASSERT(endptr - target.data() <= length);
        } else {
            // No 'e' found, so it's the 'f' form. Variants of snprintf generate numbers with
            // potentially multiple digits before the '.', but without decimal exponent then. So we
            // get the final decimal point from the position of the '.'. The '.' itself takes up one
            // character. We adjust by 1 below if that gets in the way.
            decpt = decptInTarget - firstSignificant;
        }
    } else {
        // In 'f' form, there can not be an 'e', so it's enough to look for the '.'
        // (and possibly adjust by 1 below)
        decpt = decptInTarget - firstSignificant;
    }

    // Move the actual digits from the snprintf target to the actual buffer.
    if (decptInTarget > firstSignificant) {
        // First move the digits before the '.', if any
        int lengthBeforeDecpt = decptInTarget - firstSignificant;
        memcpy(buf, target.data() + firstSignificant, qMin(lengthBeforeDecpt, bufSize));
        if (eSign > decptInTarget && lengthBeforeDecpt < bufSize) {
            // Then move any remaining digits, until 'e'
            memcpy(buf + lengthBeforeDecpt, target.data() + decptInTarget + 1,
                   qMin(eSign - decptInTarget - 1, bufSize - lengthBeforeDecpt));
            // The final length of the output is the distance between the first significant digit
            // and 'e' minus 1, for the '.', except if the buffer is smaller.
            length = qMin(eSign - firstSignificant - 1, bufSize);
        } else {
            // 'e' was before the decpt or things didn't fit. Don't subtract the '.' from the length.
            length = qMin(eSign - firstSignificant, bufSize);
        }
    } else {
        if (eSign > firstSignificant) {
            // If there are any significant digits at all, they are all after the '.' now.
            // Just copy them straight away.
            memcpy(buf, target.data() + firstSignificant, qMin(eSign - firstSignificant, bufSize));

            // The decimal point was before the first significant digit, so we were one off above.
            // Consider 0.1 - buf will be just '1', and decpt should be 0. But
            // "decptInTarget - firstSignificant" will yield -1.
            ++decpt;
            length = qMin(eSign - firstSignificant, bufSize);
        } else {
            // No significant digits means the number is just 0.
            buf[0] = '0';
            length = 1;
            decpt = 1;
        }
    }
#endif // QT_NO_DOUBLECONVERSION || QT_BOOTSTRAPPED
    while (length > 1 && buf[length - 1] == '0') // drop trailing zeroes
        --length;
}

double qt_asciiToDouble(const char *num, qsizetype numLen, bool &ok, int &processed,
                        StrayCharacterMode strayCharMode)
{
    auto string_equals = [](const char *needle, const char *haystack, qsizetype haystackLen) {
        qsizetype needleLen = strlen(needle);
        return needleLen == haystackLen && memcmp(needle, haystack, haystackLen) == 0;
    };

    if (numLen == 0) {
        ok = false;
        processed = 0;
        return 0.0;
    }

    ok = true;

    // We have to catch NaN before because we need NaN as marker for "garbage" in the
    // libdouble-conversion case and, in contrast to libdouble-conversion or sscanf, we don't allow
    // "-nan" or "+nan"
    if (string_equals("nan", num, numLen)) {
        processed = 3;
        return qt_qnan();
    } else if (string_equals("+nan", num, numLen) || string_equals("-nan", num, numLen)) {
        processed = 0;
        ok = false;
        return 0.0;
    }

    // Infinity values are implementation defined in the sscanf case. In the libdouble-conversion
    // case we need infinity as overflow marker.
    if (string_equals("+inf", num, numLen)) {
        processed = 4;
        return qt_inf();
    } else if (string_equals("inf", num, numLen)) {
        processed = 3;
        return qt_inf();
    } else if (string_equals("-inf", num, numLen)) {
        processed = 4;
        return -qt_inf();
    }

    double d = 0.0;
#if !defined(QT_NO_DOUBLECONVERSION) && !defined(QT_BOOTSTRAPPED)
    int conv_flags = double_conversion::StringToDoubleConverter::NO_FLAGS;
    if (strayCharMode == TrailingJunkAllowed) {
        conv_flags = double_conversion::StringToDoubleConverter::ALLOW_TRAILING_JUNK;
    } else if (strayCharMode == WhitespacesAllowed) {
        conv_flags = double_conversion::StringToDoubleConverter::ALLOW_LEADING_SPACES
                | double_conversion::StringToDoubleConverter::ALLOW_TRAILING_SPACES;
    }
    double_conversion::StringToDoubleConverter conv(conv_flags, 0.0, qt_qnan(), nullptr, nullptr);
    if (int(numLen) != numLen) {
        // a number over 2 GB in length is silly, just assume it isn't valid
        ok = false;
        processed = 0;
        return 0.0;
    } else {
        d = conv.StringToDouble(num, numLen, &processed);
    }

    if (!qIsFinite(d)) {
        ok = false;
        if (qIsNaN(d)) {
            // Garbage found. We don't accept it and return 0.
            processed = 0;
            return 0.0;
        } else {
            // Overflow. That's not OK, but we still return infinity.
            return d;
        }
    }
#else
    // need to ensure that our input is null-terminated for sscanf
    // (this is a QVarLengthArray<char, 128> but this code here is too low-level for QVLA)
    char reasonableBuffer[128];
    char *buffer;
    if (numLen < qsizetype(sizeof(reasonableBuffer)) - 1)
        buffer = reasonableBuffer;
    else
        buffer = static_cast<char *>(malloc(numLen + 1));
    Q_CHECK_PTR(buffer);
    memcpy(buffer, num, numLen);
    buffer[numLen] = '\0';

    if (qDoubleSscanf(buffer, QT_CLOCALE, "%lf%n", &d, &processed) < 1)
        processed = 0;

    if (buffer != reasonableBuffer)
        free(buffer);

    if ((strayCharMode == TrailingJunkProhibited && processed != numLen) || qIsNaN(d)) {
        // Implementation defined nan symbol or garbage found. We don't accept it.
        processed = 0;
        ok = false;
        return 0.0;
    }

    if (!qIsFinite(d)) {
        // Overflow. Check for implementation-defined infinity symbols and reject them.
        // We assume that any infinity symbol has to contain a character that cannot be part of a
        // "normal" number (that is 0-9, ., -, +, e).
        ok = false;
        for (int i = 0; i < processed; ++i) {
            char c = num[i];
            if ((c < '0' || c > '9') && c != '.' && c != '-' && c != '+' && c != 'e' && c != 'E') {
                // Garbage found
                processed = 0;
                return 0.0;
            }
        }
        return d;
    }
#endif // !defined(QT_NO_DOUBLECONVERSION) && !defined(QT_BOOTSTRAPPED)

    // Otherwise we would have gotten NaN or sorted it out above.
    Q_ASSERT(strayCharMode == TrailingJunkAllowed || processed == numLen);

    // Check if underflow has occurred.
    if (isZero(d)) {
        for (int i = 0; i < processed; ++i) {
            if (num[i] >= '1' && num[i] <= '9') {
                // if a digit before any 'e' is not 0, then a non-zero number was intended.
                ok = false;
                return 0.0;
            } else if (num[i] == 'e' || num[i] == 'E') {
                break;
            }
        }
    }
    return d;
}

unsigned long long
qstrtoull(const char * nptr, const char **endptr, int base, bool *ok)
{
    // strtoull accepts negative numbers. We don't.
    // Use a different variable so we pass the original nptr to strtoul
    // (we need that so endptr may be nptr in case of failure)
    const char *begin = nptr;
    while (ascii_isspace(*begin))
        ++begin;
    if (*begin == '-') {
        *ok = false;
        return 0;
    }

    *ok = true;
    errno = 0;
    char *endptr2 = nullptr;
    unsigned long long result = qt_strtoull(nptr, &endptr2, base);
    if (endptr)
        *endptr = endptr2;
    if ((result == 0 || result == std::numeric_limits<unsigned long long>::max())
            && (errno || endptr2 == nptr)) {
        *ok = false;
        return 0;
    }
    return result;
}

long long
qstrtoll(const char * nptr, const char **endptr, int base, bool *ok)
{
    *ok = true;
    errno = 0;
    char *endptr2 = nullptr;
    long long result = qt_strtoll(nptr, &endptr2, base);
    if (endptr)
        *endptr = endptr2;
    if ((result == 0 || result == std::numeric_limits<long long>::min()
         || result == std::numeric_limits<long long>::max())
            && (errno || nptr == endptr2)) {
        *ok = false;
        return 0;
    }
    return result;
}

static Q_ALWAYS_INLINE void qulltoBasicLatin_helper(qulonglong number, int base, char16_t *&p)
{
    // Performance-optimized code. Compiler can generate faster code when base is known.
    switch (base) {
#define BIG_BASE_LOOP(b)                        \
    do {                                        \
        const int r = number % b;               \
        *--p = (r < 10 ? u'0' : u'a' - 10) + r; \
        number /= b;                            \
    } while (number)
#ifndef __OPTIMIZE_SIZE__
#define SMALL_BASE_LOOP(b)        \
    do {                          \
        *--p = u'0' + number % b; \
        number /= b;              \
    } while (number)

    case 2: SMALL_BASE_LOOP(2); break;
    case 8: SMALL_BASE_LOOP(8); break;
    case 10: SMALL_BASE_LOOP(10); break;
    case 16: BIG_BASE_LOOP(16); break;
#undef SMALL_BASE_LOOP
#endif
    default: BIG_BASE_LOOP(base); break;
#undef BIG_BASE_LOOP
    }
}

// This is technically "qulonglong to ascii", but that name's taken
QString qulltoBasicLatin(qulonglong number, int base, bool negative)
{
    if (number == 0)
        return QStringLiteral("0");
    // Length of MIN_LLONG with the sign in front is 65; we never need surrogate pairs.
    // We do not need a terminator.
    const unsigned maxlen = 65;
    static_assert(CHAR_BIT * sizeof(number) + 1 <= maxlen);
    char16_t buff[maxlen];
    char16_t *const end = buff + maxlen, *p = end;

    qulltoBasicLatin_helper(number, base, p);
    if (negative)
        *--p = u'-';

    return QString(reinterpret_cast<QChar *>(p), end - p);
}

QString qulltoa(qulonglong number, int base, const QStringView zero)
{
    // Length of MAX_ULLONG in base 2 is 64; and we may need a surrogate pair
    // per digit. We do not need a terminator.
    const unsigned maxlen = 128;
    static_assert(CHAR_BIT * sizeof(number) <= maxlen);
    char16_t buff[maxlen];
    char16_t *const end = buff + maxlen, *p = end;

    if (base != 10 || zero == u"0") {
        qulltoBasicLatin_helper(number, base, p);
    } else if (zero.size() && !zero.at(0).isSurrogate()) {
        const char16_t zeroUcs2 = zero.at(0).unicode();
        while (number != 0) {
            *(--p) = unicodeForDigit(number % base, zeroUcs2);

            number /= base;
        }
    } else if (zero.size() == 2 && zero.at(0).isHighSurrogate()) {
        const char32_t zeroUcs4 = QChar::surrogateToUcs4(zero.at(0), zero.at(1));
        while (number != 0) {
            const char32_t digit = unicodeForDigit(number % base, zeroUcs4);

            *(--p) = QChar::lowSurrogate(digit);
            *(--p) = QChar::highSurrogate(digit);

            number /= base;
        }
    } else { // zero should always be either a non-surrogate or a surrogate pair:
        Q_UNREACHABLE();
        return QString();
    }

    return QString(reinterpret_cast<QChar *>(p), end - p);
}

/*!
  \internal

  Converts the initial portion of the string pointed to by \a s00 to a double, using the 'C' locale.
 */
double qstrntod(const char *s00, qsizetype len, const char **se, bool *ok)
{
    int processed = 0;
    bool nonNullOk = false;
    double d = qt_asciiToDouble(s00, len, nonNullOk, processed, TrailingJunkAllowed);
    if (se)
        *se = s00 + processed;
    if (ok)
        *ok = nonNullOk;
    return d;
}

QString qdtoa(qreal d, int *decpt, int *sign)
{
    bool nonNullSign = false;
    int nonNullDecpt = 0;
    int length = 0;

    // Some versions of libdouble-conversion like an extra digit, probably for '\0'
    constexpr int digits = std::numeric_limits<double>::max_digits10 + 1;
    char result[digits];
    qt_doubleToAscii(d, QLocaleData::DFSignificantDigits, QLocale::FloatingPointShortest,
                     result, digits, nonNullSign, length, nonNullDecpt);

    if (sign)
        *sign = nonNullSign ? 1 : 0;
    if (decpt)
        *decpt = nonNullDecpt;

    return QLatin1String(result, length);
}

QT_END_NAMESPACE
