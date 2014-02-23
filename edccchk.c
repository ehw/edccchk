////////////////////////////////////////////////////////////////////////////////
//
#define TITLE "edccchk - CD image EDC/ECC Checker"
#define COPYR "Copyright (C) 2013 Natalia Portillo"
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.
//
////////////////////////////////////////////////////////////////////////////////

#include "common.h"
#include "banner.h"

////////////////////////////////////////////////////////////////////////////////
//
// Sector types
//
// Mode 1
// -----------------------------------------------------
//        0  1  2  3  4  5  6  7  8  9  A  B  C  D  E  F
// 0000h 00 FF FF FF FF FF FF FF FF FF FF 00 [-ADDR-] 01
// 0010h [---DATA...
// ...
// 0800h                                     ...DATA---]
// 0810h [---EDC---] 00 00 00 00 00 00 00 00 [---ECC...
// ...
// 0920h                                      ...ECC---]
// -----------------------------------------------------
//
// Mode 2 (XA), form 1
// -----------------------------------------------------
//        0  1  2  3  4  5  6  7  8  9  A  B  C  D  E  F
// 0000h 00 FF FF FF FF FF FF FF FF FF FF 00 [-ADDR-] 02
// 0010h [--FLAGS--] [--FLAGS--] [---DATA...
// ...
// 0810h             ...DATA---] [---EDC---] [---ECC...
// ...
// 0920h                                      ...ECC---]
// -----------------------------------------------------
//
// Mode 2 (XA), form 2
// -----------------------------------------------------
//        0  1  2  3  4  5  6  7  8  9  A  B  C  D  E  F
// 0000h 00 FF FF FF FF FF FF FF FF FF FF 00 [-ADDR-] 02
// 0010h [--FLAGS--] [--FLAGS--] [---DATA...
// ...
// 0920h                         ...DATA---] [---EDC---]
// -----------------------------------------------------
//
// ADDR:  Sector address, encoded as minutes:seconds:frames in BCD
// FLAGS: Used in Mode 2 (XA) sectors describing the type of sector; repeated
//        twice for redundancy
// DATA:  Area of the sector which contains the actual data itself
// EDC:   Error Detection Code
// ECC:   Error Correction Code
//

////////////////////////////////////////////////////////////////////////////////

static uint32_t get32lsb(const uint8_t* src) {
    return
        (((uint32_t)(src[0])) <<  0) |
        (((uint32_t)(src[1])) <<  8) |
        (((uint32_t)(src[2])) << 16) |
        (((uint32_t)(src[3])) << 24);
}

static uint32_t nondatasectors;
static uint32_t mode0sectors;
static uint32_t mode0errors;
static uint32_t mode1sectors;
static uint32_t mode1errors;
static uint32_t mode2f1sectors;
static uint32_t mode2f1errors;
static uint32_t mode2f2sectors;
static uint32_t mode2f2errors;
static uint32_t totalsectors;
static uint32_t totalerrors;

////////////////////////////////////////////////////////////////////////////////
//
// LUTs used for computing ECC/EDC
//
static uint8_t  ecc_f_lut[256];
static uint8_t  ecc_b_lut[256];
static uint32_t edc_lut  [256];

static void eccedc_init(void) {
    size_t i;
    for(i = 0; i < 256; i++) {
        uint32_t edc = i;
        size_t j = (i << 1) ^ (i & 0x80 ? 0x11D : 0);
        ecc_f_lut[i] = j;
        ecc_b_lut[i ^ j] = i;
        for(j = 0; j < 8; j++) {
            edc = (edc >> 1) ^ (edc & 1 ? 0xD8018001 : 0);
        }
        edc_lut[i] = edc;
    }
}

////////////////////////////////////////////////////////////////////////////////
//
// Compute EDC for a block
//
static uint32_t edc_compute(
    uint32_t edc,
    const uint8_t* src,
    size_t size
) {
    for(; size; size--) {
        edc = (edc >> 8) ^ edc_lut[(edc ^ (*src++)) & 0xFF];
    }
    return edc;
}

////////////////////////////////////////////////////////////////////////////////
//
// Check ECC block (either P or Q)
// Returns true if the ECC data is an exact match
//
static int8_t ecc_checkpq(
    const uint8_t* address,
    const uint8_t* data,
    size_t major_count,
    size_t minor_count,
    size_t major_mult,
    size_t minor_inc,
    const uint8_t* ecc
) {
    size_t size = major_count * minor_count;
    size_t major;
    for(major = 0; major < major_count; major++) {
        size_t index = (major >> 1) * major_mult + (major & 1);
        uint8_t ecc_a = 0;
        uint8_t ecc_b = 0;
        size_t minor;
        for(minor = 0; minor < minor_count; minor++) {
            uint8_t temp;
            if(index < 4) {
                temp = address[index];
            } else {
                temp = data[index - 4];
            }
            index += minor_inc;
            if(index >= size) { index -= size; }
            ecc_a ^= temp;
            ecc_b ^= temp;
            ecc_a = ecc_f_lut[ecc_a];
        }
        ecc_a = ecc_b_lut[ecc_f_lut[ecc_a] ^ ecc_b];
        if(
            ecc[major              ] != (ecc_a        ) ||
            ecc[major + major_count] != (ecc_a ^ ecc_b)
        ) {
            return 0;
        }
    }
    return 1;
}

//
// Check ECC P and Q codes for a sector
// Returns true if the ECC data is an exact match
//
static int8_t ecc_checksector(
    const uint8_t *address,
    const uint8_t *data,
    const uint8_t *ecc
) {
    return
        ecc_checkpq(address, data, 86, 24,  2, 86, ecc) &&      // P
        ecc_checkpq(address, data, 52, 43, 86, 88, ecc + 0xAC); // Q
}

////////////////////////////////////////////////////////////////////////////////

static const uint8_t zeroaddress[4] = {0, 0, 0, 0};

////////////////////////////////////////////////////////////////////////////////
//
// Check if this is a sector we can compress
//
// Sector types:
//   0: Literal bytes (not a sector)
//   1: 2352 mode 1         predict sync, mode, reserved, edc, ecc
//   2: 2336 mode 2 form 1  predict redundant flags, edc, ecc
//   3: 2336 mode 2 form 2  predict redundant flags, edc
//
static int8_t detect_sector(const uint8_t* sector, size_t size_available) {
    if(
        size_available >= 2352 &&
        sector[0x000] == 0x00 && // sync (12 bytes)
        sector[0x001] == 0xFF &&
        sector[0x002] == 0xFF &&
        sector[0x003] == 0xFF &&
        sector[0x004] == 0xFF &&
        sector[0x005] == 0xFF &&
        sector[0x006] == 0xFF &&
        sector[0x007] == 0xFF &&
        sector[0x008] == 0xFF &&
        sector[0x009] == 0xFF &&
        sector[0x00A] == 0xFF &&
        sector[0x00B] == 0x00 &&
        sector[0x00F] == 0x01 && // mode (1 byte)
        sector[0x814] == 0x00 && // reserved (8 bytes)
        sector[0x815] == 0x00 &&
        sector[0x816] == 0x00 &&
        sector[0x817] == 0x00 &&
        sector[0x818] == 0x00 &&
        sector[0x819] == 0x00 &&
        sector[0x81A] == 0x00 &&
        sector[0x81B] == 0x00
    ) {
        //
        // Might be Mode 1
        //
        if(
            ecc_checksector(
                sector + 0xC,
                sector + 0x10,
                sector + 0x81C
            ) &&
            edc_compute(0, sector, 0x810) == get32lsb(sector + 0x810)
        ) {
            return 1; // Mode 1
        }

    } else if(
        size_available >= 2336 &&
        sector[0] == sector[4] && // flags (4 bytes)
        sector[1] == sector[5] && //   versus redundant copy
        sector[2] == sector[6] &&
        sector[3] == sector[7]
    ) {
        //
        // Might be Mode 2, Form 1 or 2
        //
        if(
            ecc_checksector(
                zeroaddress,
                sector,
                sector + 0x80C
            ) &&
            edc_compute(0, sector, 0x808) == get32lsb(sector + 0x808)
        ) {
            return 2; // Mode 2, Form 1
        }
        //
        // Might be Mode 2, Form 2
        //
        if(
            edc_compute(0, sector, 0x91C) == get32lsb(sector + 0x91C)
        ) {
            return 3; // Mode 2, Form 2
        }
    }

    //
    // Nothing
    //
    return 0;
}

////////////////////////////////////////////////////////////////////////////////

static uint8_t sector_buffer[2352];

////////////////////////////////////////////////////////////////////////////////

static off_t mycounter_analyze = (off_t)-1;
static off_t mycounter_encode  = (off_t)-1;
static off_t mycounter_decode  = (off_t)-1;
static off_t mycounter_total   = 0;

static void resetcounter(off_t total) {
    mycounter_analyze = (off_t)-1;
    mycounter_encode  = (off_t)-1;
    mycounter_decode  = (off_t)-1;
    mycounter_total   = total;
}

static void encode_progress(void) {
    off_t a = (mycounter_analyze + 64) / 128;
    off_t t = (mycounter_total   + 64) / 128;
    if(!t) { t = 1; }
    fprintf(stderr,
        "Analyze(%02u%%)\r",
        (unsigned)((((off_t)100) * a) / t)
    );
}

static void setcounter_analyze(off_t n) {
    int8_t p = ((n >> 20) != (mycounter_analyze >> 20));
    mycounter_analyze = n;
    if(p) { encode_progress(); }
}

static void setcounter_encode(off_t n) {
    int8_t p = ((n >> 20) != (mycounter_encode >> 20));
    mycounter_encode = n;
    if(p) { encode_progress(); }
}

////////////////////////////////////////////////////////////////////////////////
//
// Returns nonzero on error
//
static int8_t ecmify(
    const char* infilename
) {
    int8_t returncode = 0;

    FILE* in  = NULL;

    uint8_t* queue = NULL;
    size_t queue_start_ofs = 0;
    size_t queue_bytes_available = 0;

    uint32_t input_edc = 0;

    //
    // Current sector type (run)
    //
    int8_t   curtype = -1; // not a valid type
    uint32_t curtype_count = 0;
    off_t    curtype_in_start = 0;

    uint32_t literal_skip = 0;

    off_t input_file_length;
    off_t input_bytes_checked = 0;
    off_t input_bytes_queued  = 0;

    off_t typetally[4] = {0,0,0,0};

    size_t queue_size = ((size_t)(-1)) - 4095;
    if((unsigned long)queue_size > 0x40000lu) {
        queue_size = (size_t)0x40000lu;
    }

    //
    // Allocate space for queue
    //
    queue = malloc(queue_size);
    if(!queue) {
        printf("Out of memory\n");
        goto error;
    }

    //
    // Open both files
    //
    in = fopen(infilename, "rb");
    if(!in) { goto error_in; }

    printf("Checking %s...\n", infilename);

    //
    // Get the length of the input file
    //
    if(fseeko(in, 0, SEEK_END) != 0) { goto error_in; }
    input_file_length = ftello(in);
    if(input_file_length < 0) { goto error_in; }

    resetcounter(input_file_length);
    
    nondatasectors= 0;
    mode0sectors= 0;
    mode0errors= 0;
    mode1sectors= 0;
    mode1errors= 0;
    mode2f1sectors= 0;
    mode2f1errors= 0;
    mode2f2sectors= 0;
    mode2f2errors= 0;
    totalsectors= 0;
    totalerrors= 0;

    for(;;) {
        int8_t detecttype;

        //
        // Refill queue if necessary
        //
        if(
            (queue_bytes_available < 2352) &&
            (((off_t)queue_bytes_available) < (input_file_length - input_bytes_queued))
        ) {
            //
            // We need to read more data
            //
            off_t willread = input_file_length - input_bytes_queued;
            off_t maxread = queue_size - queue_bytes_available;
            if(willread > maxread) {
                willread = maxread;
            }

            if(queue_start_ofs > 0) {
                memmove(queue, queue + queue_start_ofs, queue_bytes_available);
                queue_start_ofs = 0;
            }
            if(willread) {
                setcounter_analyze(input_bytes_queued);

                if(fseeko(in, input_bytes_queued, SEEK_SET) != 0) {
                    goto error_in;
                }
                if(fread(queue + queue_bytes_available, 1, willread, in) != (size_t)willread) {
                    goto error_in;
                }

                input_edc = edc_compute(
                    input_edc,
                    queue + queue_bytes_available,
                    willread
                );

                input_bytes_queued    += willread;
                queue_bytes_available += willread;
            }
        }

        if(queue_bytes_available == 0) {
            //
            // No data left to read -> quit
            //
            detecttype = -1;

        }
        
        uint8_t* sector = queue + queue_start_ofs;

        // Data sector
        if (
        sector[0x000] == 0x00 && // sync (12 bytes)
        sector[0x001] == 0xFF &&
        sector[0x002] == 0xFF &&
        sector[0x003] == 0xFF &&
        sector[0x004] == 0xFF &&
        sector[0x005] == 0xFF &&
        sector[0x006] == 0xFF &&
        sector[0x007] == 0xFF &&
        sector[0x008] == 0xFF &&
        sector[0x009] == 0xFF &&
        sector[0x00A] == 0xFF &&
        sector[0x00B] == 0x00
            )
        {
            // Just for debug
//            fprintf(stderr, "Address: %02X:%02X:%02X\n", sector[0x00C], sector[0x00D], sector[0x00E]);
            if(sector[0x00F] == 0x00) // mode (1 byte)
            {
                mode0sectors++;
                for(int i=0x010;i < 0x930;i++)
                {
                    if(sector[i] != 0x00)
                    {
                        mode0errors++;
                        totalerrors++;
                        fprintf(stderr, "Mode 0 sector with error at address: %02X:%02X:%02X\n", sector[0x00C], sector[0x00D], sector[0x00E]);
                        break;
                    }
                }
            }
            else if(sector[0x00F] == 0x01) // mode (1 byte)
            {
                mode1sectors++;
                if(
                   !ecc_checksector(
                                   sector + 0xC,
                                   sector + 0x10,
                                   sector + 0x81C
                                   ) ||
                   edc_compute(0, sector, 0x810) != get32lsb(sector + 0x810) ||
                   sector[0x814] != 0x00 || // reserved (8 bytes)
                   sector[0x815] != 0x00 ||
                   sector[0x816] != 0x00 ||
                   sector[0x817] != 0x00 ||
                   sector[0x818] != 0x00 ||
                   sector[0x819] != 0x00 ||
                   sector[0x81A] != 0x00 ||
                   sector[0x81B] != 0x00
                   ) {
                    mode1errors++;
                    totalerrors++;
                    fprintf(stderr, "Mode 1 sector with error at address: %02X:%02X:%02X\n", sector[0x00C], sector[0x00D], sector[0x00E]);
                }
            }
            else if(sector[0x00F] == 0x02) // mode (1 byte)
            {
                uint8_t* m2sec = sector + 0x10;
                
                if((sector[0x012] & 0x20) == 0x20) // mode 2 form 2
                {
                    mode2f2sectors++;
                    if(edc_compute(0, m2sec, 0x91C) != get32lsb(m2sec + 0x91C))
                    {
                        fprintf(stderr, "Mode 2 form 2 sector with error at address: %02X:%02X:%02X\n", sector[0x00C], sector[0x00D], sector[0x00E]);
                        mode2f2errors++;
                    }
                }
                else
                {
                    mode2f1sectors++;
                    if(
                       !ecc_checksector(
                                        zeroaddress,
                                        m2sec,
                                        m2sec + 0x80C
                                        ) ||
                       edc_compute(0, m2sec, 0x808) != get32lsb(m2sec + 0x808))
                    {
                        fprintf(stderr, "Mode 2 form 1 sector with error at address: %02X:%02X:%02X\n", sector[0x00C], sector[0x00D], sector[0x00E]);
                        mode2f1errors++;
                    }
                }
            }
            else // Unknown sector mode!!!
            {
                nondatasectors++;
            }
        }
        else // Non data sector
        {
            nondatasectors++;
        }


        //
        // Current type is negative ==> quit
        //
        if(detecttype < 0) { break; }

        //
        // Advance to the next sector
        //
        totalsectors++;
        input_bytes_checked   += 2352;
        queue_start_ofs       += 2352;
        queue_bytes_available -= 2352;
    }

    //
    // Show report
    //
    totalsectors++;
    printf("Non-data sectors........ %d\n", nondatasectors);
    printf("Mode 0 sectors.......... %d\n", mode0sectors);
    printf("\twith errors..... %d\n", mode0errors);
    printf("Mode 1 sectors.......... %d\n", mode1sectors);
    printf("\twith errors..... %d\n", mode1errors);
    printf("Mode 2 form 1 sectors... %d\n", mode2f1sectors);
    printf("\twith errors..... %d\n", mode2f1errors);
    printf("Mode 2 form 2 sectors... %d\n", mode2f2sectors);
    printf("\twith errors..... %d\n", mode2f2errors);
    printf("Total sectors........... %d\n", totalsectors);
    printf("Total errors............ %d\n", totalerrors);
    //
    // Success
    //
    printf("Done\n");
    returncode = 0;
    goto done;

error_in:
    printfileerror(in, infilename);
    goto error;

error:
    returncode = 1;
    goto done;

done:
    if(queue != NULL) { free(queue); }
    if(in    != NULL) { fclose(in ); }

    return returncode;
}

int main(int argc, char** argv) {
    int returncode = 0;
    char* infilename  = NULL;

    normalize_argv0(argv[0]);

    banner();
    
    //
    // Check command line
    //
    switch(argc) {
    case 2:
        infilename  = argv[1];
        //
        // Initialize the ECC/EDC tables
        //
        eccedc_init();
        if(ecmify(infilename)) { goto error; }
        break;
    default:
        goto usage;
    }

    //
    // Success
    //
    returncode = 0;
    goto done;

usage:
    printf(
        "Usage:\n"
        "\n"
        "    edccchk cdimagefile\n"
    );

error:
    returncode = 1;
    goto done;

done:
    return returncode;
}

////////////////////////////////////////////////////////////////////////////////