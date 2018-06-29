cmake_minimum_required(VERSION 2.8)
file(WRITE ${OUTPUT} "")
file(READ ${INPUT} FILEDATA HEX)
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," FILEDATA ${FILEDATA})
file(APPEND ${OUTPUT} "const unsigned char ${VARIABLE}[] = {${FILEDATA}};\nconst unsigned ${VARIABLE}_size = sizeof(${VARIABLE});\n")