
TARGET 	:= psictl
OBJS 	:= main.o crc.o lzw_decode.o lzw_encode.o

CFLAGS 	:= -I$(PWD) -Wall -Werror -Os
LDFLAGS	:= 

all : $(TARGET)

$(TARGET) : $(OBJS)
	$(CC) $^ -o $@ $(LDFLAGS)

%.o : %.c 
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -rf $(TARGET) $(OBJS)

.PHONY: all clean
