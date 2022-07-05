#include "psi.h"
#include "lzw.h"
#include "crc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <getopt.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>


enum action_e {
    ACT_ENCODE,
    ACT_DECODE,
    ACT_NONE,
};

static void usage(void) {
    fprintf(stderr, "PSI Encoder & Decoder\n");
    fprintf(stderr, "psictl <-d | -e> [ -i <file> ] [ -o <file> ]\n");
    fprintf(stderr, "   -d          decode PSI file to XML\n");
    fprintf(stderr, "   -e          encode XML file to PSI\n");
    fprintf(stderr, "   -i          input file name, If not given, read data from stdin\n");
    fprintf(stderr, "   -o          output file name, If not given, write data to stdout\n");
    exit(-1);
}

static ssize_t read_fd(int fd, char ** bufptr, size_t bufsize) {
    ssize_t ret;
    size_t len = 0;

    *bufptr = (char*)calloc(1, bufsize);
    if(!(*bufptr)) {
        return -ENOMEM;
    }

    while(1) {
        if(0 > (ret = read(fd, (*bufptr) + len, bufsize - len)) && EINTR) {
            continue;
        } else if(ret < bufsize - len) {
            len += ret;
            //printf("ret: %ld, bufsize: %lu\n", ret, bufsize);
            break;
        } else {
            len += ret;
            bufsize <<= 1;
            *bufptr = realloc(*bufptr, bufsize);
            if(!(*bufptr)) {
                fprintf(stderr, "cant allocate memory: %lu\n", bufsize); 
                return -ENOMEM;
            }
        }
    }

    return len;
}

static int do_decode_action(int infd, int outfd) {
    int ret;
    size_t offset;
    uint32_t crc;
    uint32_t saved_crc;
    char * compresshdr;
    char * crchdr;
    uint8_t * data = NULL;
    size_t datasize;
    uint8_t * buffer = NULL;
    size_t buffsize;
    LZWDecoderState * decodeCtx = NULL;
   
    datasize = read_fd(infd, (char**)&data, 1024);
    if(datasize < 0) {
        fprintf(stderr, "failed to read data: %m\n");
        return -errno;
    }
    //printf("datasize: %ld\n", datasize);

    offset = COMPRESSED_CONFIG_HEADER_LENGTH + CRC_CONFIG_HEADER_LENGTH;

    compresshdr = (char*)data;
    if(strncmp(compresshdr, COMPRESSED_CONFIG_HEADER, strlen(COMPRESSED_CONFIG_HEADER))) {
        fprintf(stderr, "invalid compress header, should be %s\n", COMPRESSED_CONFIG_HEADER);
        ret = -EINVAL;
        goto out;
    }
    sscanf(compresshdr + strlen(COMPRESSED_CONFIG_HEADER), "%lu", &buffsize);
    if(buffsize != datasize - offset) {
        fprintf(stderr, "invalid compressed data len\n");
        ret = -EINVAL;
        goto out;
    }

    crchdr = (char*)&data[COMPRESSED_CONFIG_HEADER_LENGTH];
    if(strncmp(crchdr, CRC_CONFIG_HEADER, strlen(CRC_CONFIG_HEADER))) {
        fprintf(stderr, "invalid crc header, should be %s\n", CRC_CONFIG_HEADER);
        ret = -EINVAL;
        goto out;
    }
    
    sscanf(crchdr + strlen(CRC_CONFIG_HEADER), "%x", &saved_crc);
    memset(crchdr, 0, CRC_CONFIG_HEADER_LENGTH);
    crc = (uint32_t)crc32(CRC_INITIAL_VALUE, &data[offset], datasize - offset);
    if(saved_crc != crc ) {
        fprintf(stderr, "CRC validate failed, %x !=  %x\n", crc, saved_crc);
        ret = -EINVAL;
        goto out;
    }

    buffsize = datasize * 10;
    buffer = calloc(1, buffsize);
    if(!buffer) {
        fprintf(stderr, "failed to allocate memory: %m\n");
        ret = -errno;
        goto out;
    }

    if(0 > (ret = lzw_init_decoder(&decodeCtx, &data[offset], datasize - offset))) {
        fprintf(stderr, "failed to init decoder: %m\n");
        goto out;
    }
 
    if(0 > (datasize = lzw_decode(decodeCtx, buffer, buffsize))) {
        fprintf(stderr, "failed to decode data: %m\n");
        ret = -1;
        goto out;
    }

    //printf("decoded datasize: %lu\n", datasize); 
    while(0 > (ret = write(outfd, buffer, datasize)) && ret == EINTR);
    if(ret < 0) {
        fprintf(stderr, "failed to write data: %m\n");
        goto out;
    }

    ret = 0;

out:
    if(data) {
        free(data);
    }
    if(buffer) {
        free(buffer);
    }
    if(decodeCtx) {
        lzw_cleanup_Decoder(&decodeCtx);
    }

    return ret;
}

static int do_encode_action(int infd, int outfd) {
    int ret;
    uint32_t crc;
    size_t offset;
    char * data = NULL;
    ssize_t datasize;
    char * buffer = NULL;
    ssize_t buffsize;
    LZWEncoderState * encodeCtx = NULL;

    datasize = read_fd(infd, (char**)&data, 1024);
    if(datasize < 0) {
        fprintf(stderr, "failed to read data: %m\n");
        return -errno;
    }

    buffsize = datasize * 2;
    buffer = calloc(1, buffsize);
    if(!buffer) {
        fprintf(stderr, "failed to allocate memory: %m\n");
        ret = -errno;
        goto out;
    }

    offset = COMPRESSED_CONFIG_HEADER_LENGTH + CRC_CONFIG_HEADER_LENGTH;
    if(0 > (ret = lzw_init_encoder(&encodeCtx, 
                    (uint8_t*)&buffer[offset], buffsize - offset))) 
    {
        fprintf(stderr, "failed to init decoder: %m\n");
        goto out;
    }

    //printf("%s : %lu\n", data, datasize);
    if(0 > (datasize = lzw_encode(encodeCtx, (uint8_t*)data, datasize))) {
        fprintf(stderr, "failed to encode data\n");
        ret = -1;
        goto out;
    }
    
    if(0 > (datasize += lzw_flush_encoder(encodeCtx))) {
        fprintf(stderr, "failed to flush encode data\n");
        ret = -1;
        goto out;
    }
    
    snprintf(buffer, COMPRESSED_CONFIG_HEADER_LENGTH, "%s%lu>", 
            COMPRESSED_CONFIG_HEADER, datasize);

    crc = crc32(CRC_INITIAL_VALUE, (uint8_t*)&buffer[offset], datasize);
    snprintf(&buffer[COMPRESSED_CONFIG_HEADER_LENGTH], 
            CRC_CONFIG_HEADER_LENGTH, "%s0x%x>", CRC_CONFIG_HEADER, crc);

    while(0 > (ret = write(outfd, buffer, datasize + offset)) && ret == EINTR);
    if(ret < 0) {
        fprintf(stderr, "failed to write data: %m\n");
        goto out;
    }

    ret = 0;

out:
    if(data) {
        free(data);
    }
    if(buffer) {
        free(buffer);
    }
    if(encodeCtx) {
        lzw_cleanup_encoder(&encodeCtx);
    }

    return ret;
}


int main(int argc, char * argv[]) {
    int opcode;
    int action = ACT_NONE;
    int infd = STDIN_FILENO;
    int outfd = STDOUT_FILENO;
    const char * output = NULL;
    const char * input = NULL;

    while(-1 != (opcode = getopt(argc, argv, "hdei:o:"))) {
        switch(opcode) {
            case 'd':
                action = ACT_DECODE;
                break;

            case 'e':
                action = ACT_ENCODE;
                break;

            case 'i':
                input = optarg;
                break;

            case 'o':
                output = optarg;
                break;
            
            case 'h':
            case '?':
            default:
                usage();
                break;
        }
    }
    
    if(action == ACT_NONE) {
        usage();
    }

    if(input) {
        infd = open(input, O_RDONLY);
        if(infd < 0) {
            fprintf(stderr, "failed to open %s: %m\n", input);
            return -ENOENT;
        }
    }

    if(output) {
        outfd = open(output, O_WRONLY|O_CREAT|O_TRUNC, 0666);
        if(outfd < 0) {
            fprintf(stderr, "failed to open %s: %m\n", output);
            return -ENOENT;
        }
    }

    if(action == ACT_DECODE) {
        do_decode_action(infd, outfd);
    } else if(action == ACT_ENCODE) {
        do_encode_action(infd, outfd);
    }

    if(input) {
        close(infd);
    }

    if(output) {
        close(outfd);
    }

    return 0;
}
