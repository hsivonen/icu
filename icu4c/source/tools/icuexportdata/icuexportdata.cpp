// © 2016 and later: Unicode, Inc. and others.
// License & terms of use: http://www.unicode.org/copyright.html

#include <iostream>
#include <vector>
#include "toolutil.h"
#include "uoptions.h"
#include "cmemory.h"
#include "charstr.h"
#include "cstring.h"
#include "unicode/uchar.h"
#include "unicode/errorcode.h"
#include "unicode/uniset.h"
#include "unicode/putil.h"
#include "unicode/umutablecptrie.h"
#include "unicode/normalizer2.h"
#include "writesrc.h"

U_NAMESPACE_USE

/*
 * Global - verbosity
 */
UBool VERBOSE = FALSE;
UBool QUIET = FALSE;

UBool haveCopyright = TRUE;
UCPTrieType trieType = UCPTRIE_TYPE_SMALL;
const char* destdir = "";

void handleError(ErrorCode& status, const char* context) {
    if (status.isFailure()) {
        std::cerr << "Error: " << context << ": " << status.errorName() << std::endl;
        exit(status.reset());
    }
}

class PropertyValueNameGetter : public ValueNameGetter {
public:
    PropertyValueNameGetter(UProperty prop) : property(prop) {}
    ~PropertyValueNameGetter() override;
    const char *getName(uint32_t value) override {
        return u_getPropertyValueName(property, value, U_SHORT_PROPERTY_NAME);
    }

private:
    UProperty property;
};

PropertyValueNameGetter::~PropertyValueNameGetter() {}

void dumpBinaryProperty(UProperty uproperty, FILE* f) {
    IcuToolErrorCode status("icuexportdata: dumpBinaryProperty");
    const char* fullPropName = u_getPropertyName(uproperty, U_LONG_PROPERTY_NAME);
    const char* shortPropName = u_getPropertyName(uproperty, U_SHORT_PROPERTY_NAME);
    const USet* uset = u_getBinaryPropertySet(uproperty, status);
    handleError(status, fullPropName);

    fputs("[[binary_property]]\n", f);
    fprintf(f, "long_name = \"%s\"\n", fullPropName);
    if (shortPropName) fprintf(f, "short_name = \"%s\"\n", shortPropName);
    usrc_writeUnicodeSet(f, uset, UPRV_TARGET_SYNTAX_TOML);
}

void dumpEnumeratedProperty(UProperty uproperty, FILE* f) {
    IcuToolErrorCode status("icuexportdata: dumpEnumeratedProperty");
    const char* fullPropName = u_getPropertyName(uproperty, U_LONG_PROPERTY_NAME);
    const char* shortPropName = u_getPropertyName(uproperty, U_SHORT_PROPERTY_NAME);
    const UCPMap* umap = u_getIntPropertyMap(uproperty, status);
    handleError(status, fullPropName);

    fputs("[[enum_property]]\n", f);
    fprintf(f, "long_name = \"%s\"\n", fullPropName);
    if (shortPropName) fprintf(f, "short_name = \"%s\"\n", shortPropName);
    PropertyValueNameGetter valueNameGetter(uproperty);
    usrc_writeUCPMap(f, umap, &valueNameGetter, UPRV_TARGET_SYNTAX_TOML);
    fputs("\n", f);

    U_ASSERT(u_getIntPropertyMinValue(uproperty) >= 0);
    int32_t maxValue = u_getIntPropertyMaxValue(uproperty);
    U_ASSERT(maxValue >= 0);
    UCPTrieValueWidth width = UCPTRIE_VALUE_BITS_32;
    if (maxValue <= 0xff) {
        width = UCPTRIE_VALUE_BITS_8;
    } else if (maxValue <= 0xffff) {
        width = UCPTRIE_VALUE_BITS_16;
    }
    LocalUMutableCPTriePointer builder(umutablecptrie_fromUCPMap(umap, status));
    LocalUCPTriePointer utrie(umutablecptrie_buildImmutable(
        builder.getAlias(),
        trieType,
        width,
        status));
    handleError(status, fullPropName);

    fputs("[enum_property.code_point_trie]\n", f);
    usrc_writeUCPTrie(f, shortPropName, utrie.getAlias(), UPRV_TARGET_SYNTAX_TOML);
}

FILE* prepareOutputFile(const char* basename) {
    IcuToolErrorCode status("icuexportdata");
    CharString outFileName;
    if (destdir != nullptr && *destdir != 0) {
        outFileName.append(destdir, status).ensureEndsWithFileSeparator(status);
    }
    outFileName.append(basename, status);
    outFileName.append(".toml", status);
    handleError(status, basename);

    FILE* f = fopen(outFileName.data(), "w");
    if (f == nullptr) {
        std::cerr << "Unable to open file: " << outFileName.data() << std::endl;
        exit(U_FILE_ACCESS_ERROR);
    }
    if (!QUIET) {
        std::cout << "Writing to: " << outFileName.data() << std::endl;
    }

    if (haveCopyright) {
        usrc_writeCopyrightHeader(f, "#", 2021);
    }
    usrc_writeFileNameGeneratedBy(f, "#", basename, "icuexportdata.cpp");

    return f;
}

// Dumps data for canonical decompostitions
void dumpDecompositions() {
    IcuToolErrorCode status("icuexportdata: dumpDecompositions");
    const char* basename = "decompositions";
    FILE* f = prepareOutputFile(basename);
    // Zero is a magic number that means decomposes to itself.
    LocalUMutableCPTriePointer builder(umutablecptrie_open(0, 0, status));
    const Normalizer2* norm = Normalizer2::getNFDInstance(status);
    // Max length as of Unicode 14 is 4, but let's make the buffer size
    // 7, since the bit handling below can deal with lengths that long.
    const int32_t DECOMPOSITION_BUFFER_SIZE = 7;
    UChar32 utf32[DECOMPOSITION_BUFFER_SIZE];
    std::vector<uint32_t> storage32;
    std::vector<uint16_t> storage16;
    // Iterate over all scalar values excluding Hangul syllables.
    for (UChar32 c = 0; c <= 0x10FFFF; ++c) {
        if (c >= 0xAC00 && c <= 0xD7A3) {
            // Hangul syllable
            continue;
        }
        if (c >= 0xD800 && c < 0xE000) {
            // Surrogate
            continue;
        }
        UnicodeString src;
        UnicodeString dst;
        src.append(c);
        norm->normalize(src, dst, status);
        if (src == dst) {
            continue;
        }
        int32_t len = dst.toUTF32(utf32, DECOMPOSITION_BUFFER_SIZE, status);
        if (len == 0 || len > 4) { // 4 is max as of Unicode 14
            status.set(U_INTERNAL_PROGRAM_ERROR);
            handleError(status, basename);
        } else if (len == 1 && utf32[0] <= 0xFFFF) {
            umutablecptrie_set(builder.getAlias(), c, utf32[0] << 16, status);
        } else if (len == 2 && utf32[0] <= 0xFFFF && utf32[1] <= 0xFFFF) {
            umutablecptrie_set(builder.getAlias(), c, (utf32[0] << 16) | utf32[1], status);
        } else {
            UBool astral = FALSE;
            for (int32_t i = 0; i < len; ++i) {
                if (utf32[i] > 0xFFFF) {
                    astral = TRUE;
                }
                if (utf32[i] == 0) {
                    status.set(U_INTERNAL_PROGRAM_ERROR);
                    handleError(status, basename);                
                }
            }
            // Format for 16-bit value:
            // Three highest bits: length (always makes the whole thing non-zero)
            // Fourth-highes bit: 0 if 16-bit units, 1 if 32-bit units
            // Lower bits: Start index in storage
            uint32_t descriptor = uint32_t(astral) << 12;
            descriptor |= uint32_t(len) << 13;
            size_t index;
            if (astral) {
                index = storage32.size();
            } else {
                index = storage16.size();
            }
            if (index > 0xFFF) {
                status.set(U_INTERNAL_PROGRAM_ERROR);
                handleError(status, basename);                
            }
            descriptor |= uint32_t(index);
            if (!descriptor || descriptor > 0xFFFF) {
                // The code above is somehow broken
                status.set(U_INTERNAL_PROGRAM_ERROR);
                handleError(status, basename);                
            }
            if (astral) {
                for (int32_t i = 0; i < len; ++i) {
                    storage32.push_back(uint32_t(utf32[i]));
                }
            } else {
                for (int32_t i = 0; i < len; ++i) {
                    storage16.push_back(uint16_t(utf32[i]));
                }
            }
            umutablecptrie_set(builder.getAlias(), c, descriptor, status);
        }
    }
    LocalUCPTriePointer utrie(umutablecptrie_buildImmutable(
        builder.getAlias(),
        trieType,
        UCPTRIE_VALUE_BITS_32,
        status));
    handleError(status, basename);
    usrc_writeArray(f, "scalars16 = [\n  ", storage16.data(), 16, storage16.size(), "  ", "\n]\n");
    usrc_writeArray(f, "scalars32 = [\n  ", storage32.data(), 32, storage32.size(), "  ", "\n]\n");
    fputs("[trie]\n", f);
    usrc_writeUCPTrie(f, "decompositions", utrie.getAlias(), UPRV_TARGET_SYNTAX_TOML);
}

enum {
    OPT_HELP_H,
    OPT_HELP_QUESTION_MARK,
    OPT_MODE,
    OPT_TRIE_TYPE,
    OPT_VERSION,
    OPT_DESTDIR,
    OPT_ALL,
    OPT_INDEX,
    OPT_COPYRIGHT,
    OPT_VERBOSE,
    OPT_QUIET,

    OPT_COUNT
};

#define UOPTION_MODE UOPTION_DEF("mode", 'm', UOPT_REQUIRES_ARG)
#define UOPTION_TRIE_TYPE UOPTION_DEF("trie-type", '\1', UOPT_REQUIRES_ARG)
#define UOPTION_ALL UOPTION_DEF("all", '\1', UOPT_NO_ARG)
#define UOPTION_INDEX UOPTION_DEF("index", '\1', UOPT_NO_ARG)

static UOption options[]={
    UOPTION_HELP_H,
    UOPTION_HELP_QUESTION_MARK,
    UOPTION_MODE,
    UOPTION_TRIE_TYPE,
    UOPTION_VERSION,
    UOPTION_DESTDIR,
    UOPTION_ALL,
    UOPTION_INDEX,
    UOPTION_COPYRIGHT,
    UOPTION_VERBOSE,
    UOPTION_QUIET,
};

int main(int argc, char* argv[]) {
    U_MAIN_INIT_ARGS(argc, argv);

    /* preset then read command line options */
    options[OPT_DESTDIR].value=u_getDataDirectory();
    argc=u_parseArgs(argc, argv, UPRV_LENGTHOF(options), options);

    if(options[OPT_VERSION].doesOccur) {
        printf("icuexportdata version %s, ICU tool to dump data files for external consumers\n",
               U_ICU_DATA_VERSION);
        printf("%s\n", U_COPYRIGHT_STRING);
        exit(0);
    }

    /* error handling, printing usage message */
    if(argc<0) {
        fprintf(stderr,
            "error in command line argument \"%s\"\n",
            argv[-argc]);
    } else if(argc<2) {
        argc=-1;
    }

    /* get the options values */
    haveCopyright = options[OPT_COPYRIGHT].doesOccur;
    destdir = options[OPT_DESTDIR].value;
    VERBOSE = options[OPT_VERBOSE].doesOccur;
    QUIET = options[OPT_QUIET].doesOccur;

    // Load list of Unicode properties
    std::vector<const char*> propNames;
    for (int i=1; i<argc; i++) {
        propNames.push_back(argv[i]);
    }
    if (options[OPT_ALL].doesOccur) {
        for (int i=UCHAR_BINARY_START; i<UCHAR_INT_LIMIT; i++) {
            if (i == UCHAR_BINARY_LIMIT) {
                i = UCHAR_INT_START;
            }
            UProperty uprop = static_cast<UProperty>(i);
            const char* propName = u_getPropertyName(uprop, U_SHORT_PROPERTY_NAME);
            if (propName == NULL) {
                propName = u_getPropertyName(uprop, U_LONG_PROPERTY_NAME);
                if (propName != NULL && VERBOSE) {
                    std::cerr << "Note: falling back to long name for: " << propName << std::endl;
                }
            }
            if (propName != NULL) {
                propNames.push_back(propName);
            }
        }
    }

    if (propNames.empty()
            || options[OPT_HELP_H].doesOccur
            || options[OPT_HELP_QUESTION_MARK].doesOccur
            || !options[OPT_MODE].doesOccur) {
        FILE *stdfile=argc<0 ? stderr : stdout;
        fprintf(stdfile,
            "usage: %s -m uprops [-options] [--all | properties...]\n"
            "\tdump Unicode property data to .toml files\n"
            "options:\n"
            "\t-h or -? or --help  this usage text\n"
            "\t-V or --version     show a version message\n"
            "\t-m or --mode        mode: currently only 'uprops', but more may be added\n"
            "\t      --trie-type   set the trie type (small or fast, default small)\n"
            "\t-d or --destdir     destination directory, followed by the path\n"
            "\t      --all         write out all properties known to icuexportdata\n"
            "\t      --index       write an _index.toml summarizing all data exported\n"
            "\t-c or --copyright   include a copyright notice\n"
            "\t-v or --verbose     Turn on verbose output\n"
            "\t-q or --quiet       do not display warnings and progress\n",
            argv[0]);
        return argc<0 ? U_ILLEGAL_ARGUMENT_ERROR : U_ZERO_ERROR;
    }

    const char* mode = options[OPT_MODE].value;
    if (uprv_strcmp(mode, "decompositions") == 0) {
        dumpDecompositions();
        return 0;
    }

    if (uprv_strcmp(mode, "uprops") != 0) {
        fprintf(stderr, "Invalid option for --mode (must be uprops)\n");
        return U_ILLEGAL_ARGUMENT_ERROR;
    }

    if (options[OPT_TRIE_TYPE].doesOccur) {
        if (uprv_strcmp(options[OPT_TRIE_TYPE].value, "fast") == 0) {
            trieType = UCPTRIE_TYPE_FAST;
        } else if (uprv_strcmp(options[OPT_TRIE_TYPE].value, "small") == 0) {
            trieType = UCPTRIE_TYPE_SMALL;
        } else {
            fprintf(stderr, "Invalid option for --trie-type (must be small or fast)\n");
            return U_ILLEGAL_ARGUMENT_ERROR;
        }
    }

    for (const char* propName : propNames) {
        UProperty propEnum = u_getPropertyEnum(propName);
        if (propEnum == UCHAR_INVALID_CODE) {
            std::cerr << "Error: Invalid property alias: " << propName << std::endl;
            return U_ILLEGAL_ARGUMENT_ERROR;
        }

        FILE* f = prepareOutputFile(propName);

        UVersionInfo versionInfo;
        u_getUnicodeVersion(versionInfo);
        char uvbuf[U_MAX_VERSION_STRING_LENGTH];
        u_versionToString(versionInfo, uvbuf);
        fprintf(f, "icu_version = \"%s\"\nunicode_version = \"%s\"\n\n",
            U_ICU_VERSION,
            uvbuf);

        if (propEnum < UCHAR_BINARY_LIMIT) {
            dumpBinaryProperty(propEnum, f);
        } else if (UCHAR_INT_START <= propEnum && propEnum <= UCHAR_INT_LIMIT) {
            dumpEnumeratedProperty(propEnum, f);
        } else {
            std::cerr << "Don't know how to write property: " << propEnum << std::endl;
            return U_INTERNAL_PROGRAM_ERROR;
        }

        fclose(f);
    }

    if (options[OPT_INDEX].doesOccur) {
        FILE* f = prepareOutputFile("_index");
        fprintf(f, "index = [\n");
        for (const char* propName : propNames) {
            // At this point, propName is a valid property name, so it should be alphanum ASCII
            fprintf(f, "  { filename=\"%s.toml\" },\n", propName);
        }
        fprintf(f, "]\n");
        fclose(f);
    }

    return 0;
}
