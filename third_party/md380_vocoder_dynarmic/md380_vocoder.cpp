#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <fstream>
#include <iostream>

#include "tables.h"

#include "dynarmic/interface/A32/a32.h"
#include "dynarmic/interface/A32/config.h"

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;

class MD380Environment final : public Dynarmic::A32::UserCallbacks {
public:
    u64 ticks_left = 0;
    Dynarmic::A32::Jit* cpu;
    uint8_t* firmware{}; // At 0x0800C000
    std::array<u8, 0x20000> sram{}; // At 0x20000000
    std::array<u8, 0x20000> tcram{}; // At 0x10000000
    std::array<u8, 0x10000> stack{}; // At 0x21000000

    u8 MemoryRead8(u32 vaddr) override {
        if (vaddr >= 0x0800C000 && vaddr < 0x0800C000 + 0x100000) {
            return firmware[vaddr - 0x0800C000];
        }
        if (vaddr >= 0x20000000 && vaddr < 0x20000000 + sram.size()) {
            return sram[vaddr - 0x20000000];
        }
        if (vaddr >= 0x10000000 && vaddr < 0x10000000 + tcram.size()) {
            return tcram[vaddr - 0x10000000];
        }
        if (vaddr >= 0x21000000 && vaddr < 0x21000000 + stack.size()) {
            return stack[vaddr - 0x21000000];
        }
        // #udf 0 to halt
        if (vaddr == 0x30000000) return 0xde;
        if (vaddr == 0x30000001) return 0x00;
        return 0;
    }

    u16 MemoryRead16(u32 vaddr) override {
        return u16(MemoryRead8(vaddr)) | u16(MemoryRead8(vaddr + 1)) << 8;
    }

    u32 MemoryRead32(u32 vaddr) override {
        return u32(MemoryRead16(vaddr)) | u32(MemoryRead16(vaddr + 2)) << 16;
    }

    u64 MemoryRead64(u32 vaddr) override {
        return u64(MemoryRead32(vaddr)) | u64(MemoryRead32(vaddr + 4)) << 32;
    }

    void MemoryWrite8(u32 vaddr, u8 value) override {
        if (vaddr >= 0x0800C000 && vaddr < 0x0800C000 + 0x100000) {
            firmware[vaddr - 0x0800C000] = vaddr;
            return;
        }
        if (vaddr >= 0x20000000 && vaddr < 0x20000000 + sram.size()) {
            sram[vaddr - 0x20000000] = value;
            return;
        }
        if (vaddr >= 0x10000000 && vaddr < 0x10000000 + tcram.size()) {
            tcram[vaddr - 0x10000000] = value;
            return;
        }
        if (vaddr >= 0x21000000 && vaddr < 0x21000000 + stack.size()) {
            stack[vaddr - 0x21000000] = value;
            return;
        }
    }

    void MemoryWrite16(u32 vaddr, u16 value) override {
        MemoryWrite8(vaddr, u8(value));
        MemoryWrite8(vaddr + 1, u8(value >> 8));
    }

    void MemoryWrite32(u32 vaddr, u32 value) override {
        MemoryWrite16(vaddr, u16(value));
        MemoryWrite16(vaddr + 2, u16(value >> 16));
    }

    void MemoryWrite64(u32 vaddr, u64 value) override {
        MemoryWrite32(vaddr, u32(value));
        MemoryWrite32(vaddr + 4, u32(value >> 32));
    }

    void InterpreterFallback(u32 pc, size_t num_instructions) override {
        // This is never called in practice.
        std::terminate();
    }

    void CallSVC(u32 swi) override {
    }

    void ExceptionRaised(u32 pc, Dynarmic::A32::Exception exception) override {
        cpu->HaltExecution();
    }

    void AddTicks(u64 ticks) override {
    }

    u64 GetTicksRemaining() override {
        return ticks_left;
    }
};

class MD380Emulator {
    private:
        MD380Environment env;
        Dynarmic::A32::UserConfig user_config;
        Dynarmic::A32::Jit cpu;
    public:
        MD380Emulator(uint8_t* firmware, uint8_t* sram) :
            env{}, user_config{.callbacks = &env}, cpu{user_config} {
            env.cpu = &cpu;
            env.firmware = firmware;
            std::copy(sram, sram + 0x20000, env.sram.begin());
        }

        void AmbeUnpackFrame(uint8_t* ambeFrame) {
            int ambei = 0;
            short* ambe = (short*) &env.sram[ambe_inbuffer - 0x20000000];
            for(int i = 1;i < 7;i++) { // Skip first byte.
                for(int j = 0;j < 8;j++) {
                    ambe[ambei++] = (ambeFrame[i]>>(7-j))&1; // MSBit first
                }
            }
            //ambe[ambei++]=ambeFrame[7]&1;//Final bit in its own frame as LSBit.
            ambe[ambei++]=ambeFrame[7] ? 1 : 0;
        }
        void AmbeDecodeExecute(uint32_t outbuf_addr, int slot) {
            cpu.Regs()[0] = outbuf_addr;
            cpu.Regs()[1] = 80;
            cpu.Regs()[2] = ambe_inbuffer;
            cpu.Regs()[3] = 0;

            // Rest of the arguments on the stack
            uint32_t sp = 0x21000000 + env.stack.size() - 0x1000;
            sp -= 4;
            env.MemoryWrite32(sp, ambe_mystery);
            sp -= 4;
            env.MemoryWrite32(sp, slot);
            sp -= 4;
            env.MemoryWrite32(sp, 0);

            cpu.Regs()[13] = sp; // SP
            cpu.Regs()[14] = 0x30000000; // LR
            cpu.Regs()[15] = ambe_decode_wav; // PC
            cpu.SetCpsr(0x00000030); // Thumb

            env.ticks_left = 1000000000;
            cpu.Run();
        }
        void AmbeDecodeFrame(uint8_t* ambeFrame, int16_t* audioOutput) {
            AmbeUnpackFrame(ambeFrame);
            AmbeDecodeExecute(ambe_outbuffer0, 0);
            int16_t* outbuf0 = (int16_t*) &env.sram[ambe_outbuffer0 - 0x20000000];
            std::copy(outbuf0, outbuf0 + 80, audioOutput);
            AmbeDecodeExecute(ambe_outbuffer1, 1);
            int16_t* outbuf1 = (int16_t*) &env.sram[ambe_outbuffer1 - 0x20000000];
            std::copy(outbuf1, outbuf1 + 80, audioOutput + 80);
        }
        void AmbePackFrame(int16_t* ambeUnpacked, uint8_t* ambeFrame) {
			ambeFrame[0] = 0;
			
			for(int i = 0; i < 6; ++i){
				for(int j = 0; j < 8; ++j){
					ambeFrame[i+1] |= (ambeUnpacked[(i * 8) + (7 - j)] << j);
				}
			}
			ambeFrame[7] = ambeUnpacked[48] ? 0x80 : 0;
        }
        void AmbeEncodeExecute(uint32_t inbuf_addr, int slot) {
            cpu.Regs()[0] = ambe_outbuffer;
            cpu.Regs()[1] = 0;
            cpu.Regs()[2] = inbuf_addr;
            cpu.Regs()[3] = 0x50;

            // Rest of the arguments on the stack
            uint32_t sp = 0x21000000 + env.stack.size() - 0x1000;
            sp -= 4;
            env.MemoryWrite32(sp, ambe_en_mystery);
            sp -= 4;
            env.MemoryWrite32(sp, 0x2000);
            sp -= 4;
            env.MemoryWrite32(sp, slot);
            sp -= 4;
            env.MemoryWrite32(sp, 0x1840);

            cpu.Regs()[13] = sp; // SP
            cpu.Regs()[14] = 0x30000000; // LR
            cpu.Regs()[15] = ambe_encode_thing; // PC
            cpu.SetCpsr(0x00000030); // Thumb

            env.ticks_left = 1000000000;
            cpu.Run();
        }
        void AmbeEncodeFrame(int16_t* audioInput, uint8_t* ambeFrame) {
            int16_t* inbuf0 = (int16_t*) &env.sram[wav_inbuffer0 - 0x20000000];
            int16_t* inbuf1 = (int16_t*) &env.sram[wav_inbuffer1 - 0x20000000];
            std::copy(audioInput, audioInput + 80, inbuf0);
            std::copy(audioInput + 80, audioInput + 160, inbuf1);
            int16_t* ambe = (int16_t*) &env.sram[ambe_outbuffer - 0x20000000];
            std::fill(ambe, ambe + 49, 0);
            AmbeEncodeExecute(wav_inbuffer0, 0);
            AmbeEncodeExecute(wav_inbuffer1, 1);
            AmbePackFrame(ambe, ambeFrame);
        }
};

MD380Emulator *emulator;

int md380_init()
{
	emulator = new MD380Emulator(firmware, sram);
	return 0;
}

void md380_decode(uint8_t *ambe, int16_t *pcm)
{
	uint8_t frame[8] = {0};
    memcpy(&frame[1], ambe, 7);
	emulator->AmbeDecodeFrame(frame, pcm);
}

void md380_encode(uint8_t *ambe, int16_t *pcm)
{
	uint8_t frame[8] = {0};
	emulator->AmbeEncodeFrame(pcm, frame);
	memcpy(ambe, &frame[1], 7);
}

void md380_decode_fec(uint8_t *ambe, int16_t *pcm)
{
	unsigned int a = 0U;
	unsigned int MASK = 0x800000U;
	uint8_t ambe49[9] = {0};
	uint8_t frame[8] = {0};

	for (unsigned int i = 0U; i < 24U; i++, MASK >>= 1) {
		unsigned int aPos = A_TABLE[i];
		if (READ_BIT(ambe, aPos))
			a |= MASK;
	}

	unsigned int b = 0U;
	MASK = 0x400000U;
	for (unsigned int i = 0U; i < 23U; i++, MASK >>= 1) {
		unsigned int bPos = B_TABLE[i];
		if (READ_BIT(ambe, bPos))
			b |= MASK;
	}

	unsigned int c = 0U;
	MASK = 0x1000000U;
	for (unsigned int i = 0U; i < 25U; i++, MASK >>= 1) {
		unsigned int cPos = C_TABLE[i];
		if (READ_BIT(ambe, cPos))
			c |= MASK;
	}

	a >>= 12;

	// The PRNG
	b ^= (PRNG_TABLE[a] >> 1);
	b >>= 11;

	MASK = 0x000800U;
	for (unsigned int i = 0U; i < 12U; i++, MASK >>= 1) {
		unsigned int aPos = i + 0U;
		unsigned int bPos = i + 12U;
		WRITE_BIT(ambe49, aPos, a & MASK);
		WRITE_BIT(ambe49, bPos, b & MASK);
	}

	MASK = 0x1000000U;
	for (unsigned int i = 0U; i < 25U; i++, MASK >>= 1) {
		unsigned int cPos = i + 24U;
		WRITE_BIT(ambe49, cPos, c & MASK);
	}

	md380_decode(ambe49, pcm);
}

void md380_encode_fec(uint8_t *ambe, int16_t *pcm)
{
	unsigned int aOrig = 0U;
	unsigned int bOrig = 0U;
	unsigned int cOrig = 0U;
	unsigned int MASK = 0x000800U;
	uint8_t ambe49[9] = {0};

	md380_encode(ambe49, pcm);
	
	for (unsigned int i = 0U; i < 12U; i++, MASK >>= 1) {
		unsigned int n1 = i + 0U;
		unsigned int n2 = i  + 12U;
		if (READ_BIT(ambe49, n1))
			aOrig |= MASK;
		if (READ_BIT(ambe49, n2))
			bOrig |= MASK;
	}

	MASK = 0x1000000U;
	for (unsigned int i = 0U; i < 25U; i++, MASK >>= 1) {
		unsigned int n = i + 24U;
		if (READ_BIT(ambe49, n))
			cOrig |= MASK;
	}

	unsigned int a = ENCODING_TABLE_24128[aOrig];

	// The PRNG
	unsigned int p = PRNG_TABLE[aOrig] >> 1;

	unsigned int b = ENCODING_TABLE_23127[bOrig] >> 1;
	b ^= p;

	MASK = 0x800000U;
	for (unsigned int i = 0U; i < 24U; i++, MASK >>= 1) {
		unsigned int aPos = A_TABLE[i];
		WRITE_BIT(ambe, aPos, a & MASK);
	}

	MASK = 0x400000U;
	for (unsigned int i = 0U; i < 23U; i++, MASK >>= 1) {
		unsigned int bPos = B_TABLE[i];
		WRITE_BIT(ambe, bPos, b & MASK);
	}

	MASK = 0x1000000U;
	for (unsigned int i = 0U; i < 25U; i++, MASK >>= 1) {
		unsigned int cPos = C_TABLE[i];
		WRITE_BIT(ambe, cPos, cOrig & MASK);
	}
}

// --- Addition for the QSO One production module (qso_dmr_vocoder) ---
// Frees the emulator allocated by md380_init(). The MD380Emulator type is
// file-local, so teardown has to live in this translation unit. This is the
// only change to the upstream md380_vocoder_dynarmic source; the codec logic
// above is unmodified. See ../../MODULE_NOTES.md.
void md380_deinit()
{
	delete emulator;
	emulator = nullptr;
}

