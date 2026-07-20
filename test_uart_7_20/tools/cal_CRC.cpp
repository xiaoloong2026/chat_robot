#include <iostream>
#include <sstream>
#include <string>
#include <cstdint>

int main()
{
    std::string hex =R"(01 02 03 00 02 00 04 ec c1 dd 83 9d ff 7e e6 
    75 47 f9 aa 95 b6 a2 7b 63 b4 cf 13 a0 b7 65 
    4d 7f a2 d4 a0 34 03 a5 00 bb 42 01 47 8e b3 
    83 e7 0c 78 71 dc ab 31 b7 be b7 72 4e 5a 97 
    1d 2c 23 38 b2 63 9d 84 7c 96 0e a9 06 ce 2a 
    00 78 ba 1a 92 df 9a f1 90 28 a7 67 cf e4 b9 
    b9 2e 83 ea 7d aa 64 18 f8 60 75 a9 15 14 76 
    05 48 b5 66 61 dd 5c 3a f2 9e 07 ab 69 00 bb 
    42 01 7d 37 50 ae 10 e5 c8 e5 b1 eb 4c 84 b5 
    ac dd 13 e1 d9 e0 df 4f c9 30 89 3e 47 e5 9e 
    a5 12 01 d5 29 7e 6a 17 fe 2d d6 92 b1 33 82 
    33 47 f4 41 d3 4d b0 7a 68 82 24 01 18 7f c3 
    bc 64 59 e3 25 f6 d6 b8 aa 7e cd d1 af 40 f5 
    28 ce 63 00 bb 42 01 d9 aa 93 cd be f4 29 1a 
    8b 3a 21 d3 8c 43 f1 d5 48 06 85 87 07 69 e1 
    44 dd 4d ab 6d ef 0c ff 39 43 a5 d2 75 36 63 
    db ab 1f cb d2 b9 5e 84 dd e8 76 06 77 42 45 
    00 6f 84 87 94 5f c1 b4 42 10 a6 4a e9 67 d1 
    e6 c1 01 7a a5 e7 8c 62 00 bb 42 01 df b4 13 
    cd 81 9f 65 0d ee d5 fd ff e3 66 64 1f d6 37 
    33 13 79 a1 88 53 25 be 29 65 b5 a9 df 95 d3 
    2e e6 af 4b 63 d6 6a b0 cf 9b 65 ee d0 64 7e 
    3d b7 dc 9d 11 f0 37 96 b0 14 37 84 b0 1a 87 
    b4 7b f7 02 5a 6c ef 10 40 9a 50 dd 69 00 bb 
    42 01 d9 c2 4a 7b dc 79 76 97 24 34 78 c8 39 
    d2 4c 84 83 69 12 17 28 f7 08 2c 31 46 9e 8e 
    78 9d 2c a7 73 d2 f3 05 41 6f 35 82 f0 8f d6 
    aa 9b 1c 8a ac 3c cf f7 c6 1a db 09 2f ed c7 
    a8 5e 2d 05 37 bf 37 6b 00 87 46 e9 c3 01 9f 
    eb 1b 35 00 bb 42 01 7c 1c e8 e5 02 80 2f 3f 
    9c de 0d 9d f9 11 65 df 67 c1 dd b4 fa 33 5e 
    f1 4f 33 0e f7 38 83 a7 93 35 64 29 fa 2d c5 
    3f aa 9c d1 da 2f 86 1e 92 f3 31 0a bf c6 8f 
    74 28 6e fd 9d 67 a7 d0 1f 41 94 5d 37 f7 c2 
    fe 0e d9 1e 3d 73 dc 5b 00 bb 42 01 14 6c af 
    b1 03 0e 75 b1 39 2c bf be be b7 9e 5d f4 9e 
    65 c1 f0 93 e8 ac dc 2c ac 85 d2 bf f3 9c 19 
    12 81 58 4f 5f 3b 56 34 2b 71 bf 07 fd 2b fb 
    06 85 56 6e a7 9d 16 3a 46 34 b8 86 a1 23 5f 
    e5 8e 2e 28 8b 7f 99 f4 c9 d1 c5 ab 2f 00 bb 
    42 01 3c 70 4c c0 22 c8 09 e4 d7 9f f3 70 27 
    ff b3 b8 7a b7 cd 0a 0f ab 7f bb f2 6c 8c a0 
    1f c2 9e 19 e6 2b 53 9f ca d5 13 52 d3 4b 0c 
    b4 62 64 1e 55 f1 ad 0c ca f0 38 e8 ce 7c d7 
    e0 90 18 12 d2 74 db 6b 8b 97 5d 08 83 54 c8 
    f0 f0 1a 00 bb 42 01 70 81 51 c2 1d 98 c5 d3 
    32 f5 ff 5d cf 24 19 fd 3d 8e 4d 40 13 23 db 
    39 17 67 08 18 86 79 3e 71 63 d2 ed 4c 4e 2a 
    3d 44 9f 0a 03 62 21 38 01 cd be 40 30 30 8c 
    8e 74 71 ae 20 c4 70 69 98 72 5e 6c 4e ab 83 
    81 6f 8f be 5f da a5 46 00 bb 42 01 6a 55 c6 
    59 e4 bb 3d 6b dc d3 96 8f 81 54 11 87 5b a7 
    45 06 0d 43 cf ea 2d fd 70 60 b5 6f 3f 93 16 
    6e 1c 8b 43 d2 74 3c 5f c7 f4 31 14 b6 d9 bd 
    cb c7 06 e7 0a a9 c3 ee 91 2b b7 58 00 99 4a 
    fb ce 74 0d d7 77 d2 d4 af d3 c8 92 6f 00 bb 
    42 01 78 7c 34 88 7d 4e 84 3b 67 00 a5 2b 63 
    65 17 1b ad cb 7d 18 b6 04 91 0b 8a 98 56 2f 
    41 25 9e 41 af 03 5b 3a 9c 3d 6a 1d 81 0f 39 
    6a 6c 10 5d 6f 14 b9 a3 55 06 aa 06 bc ad a9 
    cd a8 8b f2 c7 2c b4 d7 4f 91 ec 9c 11 4f 23 
    cb f9 7c 00 bb 42 01 42 c3 bf 2f 67 cf 9f 1c 
    5b 5a 01 ad d1 e4 b0 d0 0e d7 24 7b 6c 3b 80 
    e4 19 00 64 6b d4 3b 0b f7 65 fd e1 6a ae ae 
    3c fc d5 8b 8a 9c fe 5d 07 31 0e 68 18 7d 79 
    b5 d0 1e 02 e8 42 04 ee 0a af 41 9f 4c a3 38 
    28 e0 11 41 cf 74 57 a3 00 bb 42 01 0d ae 82 
    e0 36 ba fd 5d 42 33 ac b9 c8 f0 31 52 33 5d 
    bb 06 59 4d 4d 5e 62 e9 f6 41 c8)"; 
    // 把你的全部HEX粘贴到这里

    std::stringstream ss(hex);

    unsigned int value;
    uint32_t sum = 0;
    int count = 0;

    while(ss >> std::hex >> value)
    {
        sum += value;
        count++;
    }

    uint8_t check = sum & 0xff;

    std::cout
        << "byte count: "
        << std::dec
        << count
        << std::endl;

    std::cout
        << "sum: 0x"
        << std::hex
        << sum
        << std::endl;

    std::cout
        << "check: 0x"
        << std::hex
        << (int)check
        << std::endl;


    return 0;
}