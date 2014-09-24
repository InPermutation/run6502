/* lib6502.c -- MOS Technology 6502 emulator	-*- C -*- */

/* Copyright (c) 2005 Ian Piumarta
 * 
 * All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the 'Software'),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, provided that the above copyright notice(s) and this
 * permission notice appear in all copies of the Software and that both the
 * above copyright notice(s) and this permission notice appear in supporting
 * documentation.
 *
 * THE SOFTWARE IS PROVIDED 'AS IS'.  USE ENTIRELY AT YOUR OWN RISK.
 */

/* Last edited: 2005-11-02 01:18:47 by piumarta on margaux.local
 * 
 * BUGS:
 *   - RTS and RTI do not check the return address for a callback
 *   - the disassembler cannot be configured to read two bytes for BRK
 *   - architectural variations (unimplemented/extended instructions) not implemented
 *   - ANSI versions (from from gcc extensions) of the dispatch macros are missing
 *   - emulator+disassembler in same object file (library is kind of pointless)
 */

/*
 gpz fixes:
 - N flag was borked in set macros
 - fixed flags in add, sbc
*/

#include <stdio.h>
#include <stdlib.h>

#include "lib6502.h"

typedef uint8_t  byte;
typedef uint16_t word;

enum {
  flagN= (1<<7),	/* negative 	 */
  flagV= (1<<6),	/* overflow 	 */
  flagX= (1<<5),	/* unused   	 */
  flagB= (1<<4),	/* irq from brk  */
  flagD= (1<<3),	/* decimal mode  */
  flagI= (1<<2),	/* irq disable   */
  flagZ= (1<<1),	/* zero          */
  flagC= (1<<0)		/* carry         */
};

#define getN()	(P & flagN)
#define getV()	(P & flagV)
#define getB()	(P & flagB)
#define getD()	(P & flagD)
#define getI()	(P & flagI)
#define getZ()	(P & flagZ)
#define getC()	(P & flagC)

#define setNVZC(N,V,Z,C)	(P= (P & ~(flagN | flagV | flagZ | flagC)) | (((N)!=0)<<7) | (((V)!=0)<<6) | (((Z)!=0)<<1) | ((C)!=0))
#define setNZC(N,Z,C)		(P= (P & ~(flagN |         flagZ | flagC)) | (((N)!=0)<<7) |                 (((Z)!=0)<<1) | ((C)!=0))
#define setNZ(N,Z)		(P= (P & ~(flagN |         flagZ        )) | (((N)!=0)<<7) |                 (((Z)!=0)<<1)           )
#define setZ(Z)			(P= (P & ~(                flagZ        )) |                                 (((Z)!=0)<<1)           )
#define setC(C)			(P= (P & ~(                        flagC)) |                                                 ((C)!=0))

#define NAND(P, Q)	(!((P) & (Q)))

#define tick(n)
#define tickIf(p)

/* memory access (indirect if callback installed) -- ARGUMENTS ARE EVALUATED MORE THAN ONCE! */

#define putMemory(ADDR, BYTE)			\
  ( writeCallback[ADDR]				\
      ? writeCallback[ADDR](mpu, ADDR, BYTE)	\
      : (memory[ADDR]= BYTE) )

#define getMemory(ADDR)				\
  ( readCallback[ADDR]				\
      ?  readCallback[ADDR](mpu, ADDR, 0)	\
      :  memory[ADDR] )

/* stack access (always direct) */

#define push(BYTE)		(memory[0x0100 + S--]= (BYTE))
#define pop()			(memory[++S + 0x0100])

/* adressing modes (memory access direct) */

#define implied(ticks)				\
  tick(ticks);

#define immediate(ticks)			\
  tick(ticks);					\
  ea= PC++;

#define abs(ticks)				\
  tick(ticks);					\
  ea= memory[PC] + (memory[PC + 1] << 8);	\
  PC += 2;

#define relative(ticks)				\
  tick(ticks);					\
  ea= memory[PC++];				\
  if (ea & 0x80) ea -= 0x100;			\
  tickIf((ea >> 8) != (PC >> 8));

#define indirect(ticks)				\
  tick(ticks);					\
  {						\
    word tmp;					\
    tmp= memory[PC]  + (memory[PC  + 1] << 8);	\
    ea = memory[tmp] + (memory[tmp + 1] << 8);	\
    PC += 2;					\
  }

#define absx(ticks)						\
  tick(ticks);							\
  ea= memory[PC] + (memory[PC + 1] << 8);			\
  PC += 2;							\
  tickIf((ticks == 4) && ((ea >> 8) != ((ea + X) >> 8)));	\
  ea += X;

#define absy(ticks)						\
  tick(ticks);							\
  ea= memory[PC] + (memory[PC + 1] << 8);			\
  PC += 2;							\
  tickIf((ticks == 4) && ((ea >> 8) != ((ea + Y) >> 8)));	\
  ea += Y

#define zp(ticks)				\
  tick(ticks);					\
  ea= memory[PC++];

#define zpx(ticks)				\
  tick(ticks);					\
  ea= memory[PC++] + X;				\
  ea &= 0x00ff;

#define zpy(ticks)				\
  tick(ticks);					\
  ea= memory[PC++] + Y;				\
  ea &= 0x00ff;

#define indx(ticks)				\
  tick(ticks);					\
  {						\
    byte tmp= memory[PC++] + X;			\
    ea= memory[tmp] + (memory[tmp + 1] << 8);	\
  }

#define indy(ticks)						\
  tick(ticks);							\
  {								\
    byte tmp= memory[PC++];					\
    ea= memory[tmp] + (memory[tmp + 1] << 8);			\
    tickIf((ticks == 5) && ((ea >> 8) != ((ea + Y) >> 8)));	\
    ea += Y;							\
  }

#define indabsx(ticks)					\
  tick(ticks);						\
  {							\
    word tmp;						\
    tmp= memory[PC ] + (memory[PC  + 1] << 8) + X;	\
    ea = memory[tmp] + (memory[tmp + 1] << 8);		\
  }

#define indzp(ticks)					\
  tick(ticks);						\
  {							\
    byte tmp;						\
    tmp= memory[PC++];					\
    ea = memory[tmp] + (memory[tmp + 1] << 8);		\
  }

/* insns */
#if 0
#define adc(ticks, adrmode)								\
  adrmode(ticks);									\
  {											\
    byte B= getMemory(ea);								\
    if (!getD())									\
      {											\
	int c= A + B + getC();								\
	int v= (int8_t)A + (int8_t)B + getC();						\
	fetch();									\
	A= c;										\
	setNVZC((A & 0x80), (((A & 0x80) > 0) ^ (v < 0)), (A == 0), ((c & 0x100) > 0));	\
	next();										\
      }											\
    else										\
      {											\
	int l, h, s;									\
	/* inelegant & slow, but consistent with the hw for illegal digits */		\
	l= (A & 0x0F) + (B & 0x0F) + getC();						\
	h= (A & 0xF0) + (B & 0xF0);							\
	if (l >= 0x0A) { l -= 0x0A;  h += 0x10; }					\
	if (h >= 0xA0) { h -= 0xA0; }							\
	fetch();									\
	s= h | (l & 0x0F);								\
	/* only C is valid on NMOS 6502 */						\
	setNVZC(s & 0x80, !(((A ^ B) & 0x80) && ((A ^ s) & 0x80)), !s, !!(h & 0x80));	\
	A= s;										\
	tick(1);									\
	next();										\
      }											\
  }
#endif

//	setNVZC((A & 0x80), (((A & 0x80) > 0) ^ (v < 0)), (A == 0), ((c & 0x100) > 0));	\
//	printf("adc !D A:%d + B:%d + b:%d == c:%d v=%d\n",(int8_t)A,(int8_t)B,b,c,v); \
//	setNVZC( (A & 0x80), (v<-128)||(v>127) , A == 0, c > 0xff);	\

#define adc(ticks, adrmode)								\
  adrmode(ticks);									\
  {											\
    byte B= getMemory(ea);								\
    if (!getD())									\
      {											\
	int b= getC();								\
	int c= A + B + b;								\
	int v= (int8_t)A + (int8_t)B + b;						\
	fetch();									\
	A= c;										\
	setNVZC((A & 0x80), (v<-128)||(v>127), (A == 0), ((c & 0x100) > 0));	\
	next();										\
      }											\
    else										\
      {											\
	int l, h, s;									\
	/* inelegant & slow, but consistent with the hw for illegal digits */		\
	l= (A & 0x0F) + (B & 0x0F) + getC();						\
	h= (A & 0xF0) + (B & 0xF0);							\
	if (l >= 0x0A) { l -= 0x0A;  h += 0x10; }					\
	if (h >= 0xA0) { h -= 0xA0; }							\
	fetch();									\
	s= h | (l & 0x0F);								\
	/* only C is valid on NMOS 6502 */						\
	setNVZC(s & 0x80, !(((A ^ B) & 0x80) && ((A ^ s) & 0x80)), !s, !!(h & 0x80));	\
	A= s;										\
	tick(1);									\
	next();										\
      }											\
  }



#if 0
#define sbc(ticks, adrmode)								\
  adrmode(ticks);									\
  {											\
    byte B= getMemory(ea);								\
    if (!getD())									\
      {											\
	int b= 1 - (P &0x01);								\
	int c= A - B - b;								\
	int v= (int8_t)A - (int8_t) B - b;						\
	fetch();									\
	A= c;										\
	setNVZC(A & 0x100, ((A & 0x100) > 0) ^ ((v & 0x100) != 0), A == 0, c >= 0);	\
	next();										\
      }											\
    else										\
      {											\
	/* this is verbatim ADC, with a 10's complemented operand */			\
	int l, h, s;									\
	B= 0x99 - B;									\
	l= (A & 0x0F) + (B & 0x0F) + getC();						\
	h= (A & 0xF0) + (B & 0xF0);							\
	if (l >= 0x0A) { l -= 0x0A;  h += 0x10; }					\
	if (h >= 0xA0) { h -= 0xA0; }							\
	fetch();									\
	s= h | (l & 0x0F);								\
	/* only C is valid on NMOS 6502 */						\
	setNVZC(s & 0x80, !(((A ^ B) & 0x80) && ((A ^ s) & 0x80)), !s, !!(h & 0x80));	\
	A= s;										\
	tick(1);									\
	next();										\
      }											\
  }
#endif

//	printf("sbc !D v:%04x a:%04x b:%04x c:%04x\n",v,A,b,c); \
//	printf("sbc !D A:%d - B:%d - b:%d == c:%d v=%d\n",(int8_t)A,(int8_t)B,b,c,v); \
//	setNVZC( (v < 0), (v<-128)||(v>127) , A == 0, c >= 0);	\
//	setNVZC( (A < (B+b)), (v<-128)||(v>127) , c == 0, c >= 0);	\
//        setNVZC( (A & 0x80), (v<-128)||(v>127) , c == 0, c >= 0);       \

#define sbc(ticks, adrmode)								\
  adrmode(ticks);									\
  {											\
    byte B= getMemory(ea);								\
    if (!getD())									\
      {	/* not decimal mode */								\
	int b= 1 - getC();								\
	int c= A - B - b;								\
	int v= (int8_t)A - (int8_t)B - b;						\
	fetch();									\
	A= c;										\
	setNVZC( (A & 0x80), (v<-128) || (v>127) , c == 0, c >= 0);	\
	next();										\
      }											\
    else										\
      {											\
	/* this is verbatim ADC, with a 10's complemented operand */			\
	int l, h, s;									\
	printf("sbc D\n"); \
	B= 0x99 - B;									\
	l= (A & 0x0F) + (B & 0x0F) + getC();						\
	h= (A & 0xF0) + (B & 0xF0);							\
	if (l >= 0x0A) { l -= 0x0A;  h += 0x10; }					\
	if (h >= 0xA0) { h -= 0xA0; }							\
	fetch();									\
	s= h | (l & 0x0F);								\
	/* only C is valid on NMOS 6502 */						\
	setNVZC(s & 0x80, !(((A ^ B) & 0x80) && ((A ^ s) & 0x80)), !s, !!(h & 0x80));	\
	A= s;										\
	tick(1);									\
	next();										\
      }											\
  }

#if 0
#define cmpR(ticks, adrmode, R)			\
  adrmode(ticks);				\
  fetch();					\
  {						\
    byte B= getMemory(ea);			\
    byte d= R - B;				\
    setNZC(d & 0x80, !d, R >= B);		\
  }						\
  next();
#endif

#define cmpR(ticks, adrmode, R)			\
  adrmode(ticks);				\
  fetch();					\
  {						\
    byte B= getMemory(ea);			\
    int d= R - B;				\
    setNZC(d & 0x80, (d & 0xff) == 0, d >= 0);		\
  }						\
  next();

#define cmp(ticks, adrmode)	cmpR(ticks, adrmode, A)
#define cpx(ticks, adrmode)	cmpR(ticks, adrmode, X)
#define cpy(ticks, adrmode)	cmpR(ticks, adrmode, Y)

#define dec(ticks, adrmode)			\
  adrmode(ticks);				\
  fetch();					\
  {						\
    byte B= getMemory(ea);			\
    --B;					\
    putMemory(ea, B);				\
    setNZ(B & 0x80, !B);			\
  }						\
  next();

#if 0

#define decR(ticks, adrmode, R)			\
  fetch();					\
  tick(ticks);					\
  --R;						\
  setNZ(R & 0x80, R != 0)			\
  next();
  
#endif

#define decR(ticks, adrmode, R)			\
  fetch();					\
  tick(ticks);					\
  --R;						\
  setNZ( (R & 0x80) , (R == 0) );		\
  next();

#define dea(ticks, adrmode)	decR(ticks, adrmode, A)
#define dex(ticks, adrmode)	decR(ticks, adrmode, X)
#define dey(ticks, adrmode)	decR(ticks, adrmode, Y)

#define inc(ticks, adrmode)			\
  adrmode(ticks);				\
  fetch();					\
  {						\
    byte B= getMemory(ea);			\
    ++B;					\
    putMemory(ea, B);				\
    setNZ(B & 0x80, !B);			\
  }						\
  next();

#define incR(ticks, adrmode, R)			\
  fetch();					\
  tick(ticks);					\
  ++R;						\
  setNZ(R & 0x80, !R);				\
  next();

#define ina(ticks, adrmode)	incR(ticks, adrmode, A)
#define inx(ticks, adrmode)	incR(ticks, adrmode, X)
#define iny(ticks, adrmode)	incR(ticks, adrmode, Y)

#define bit(ticks, adrmode)			\
  adrmode(ticks);				\
  fetch();					\
  {						\
    byte B= getMemory(ea);			\
    P= (P & ~(flagN | flagV | flagZ))		\
      | (B & (0xC0)) | (((A & B) == 0) << 1);	\
  }						\
  next();

#define tsb(ticks, adrmode)			\
  adrmode(ticks);				\
  fetch();					\
  {						\
    byte b= getMemory(ea);			\
    b |= A;					\
    putMemory(ea, b);				\
    setZ(!b);					\
  }						\
  next();

#define trb(ticks, adrmode)			\
  adrmode(ticks);				\
  fetch();					\
  {						\
    byte b= getMemory(ea);			\
    b |= (A ^ 0xFF);				\
    putMemory(ea, b);				\
    setZ(!b);					\
  }						\
  next();

#define bitwise(ticks, adrmode, op)		\
  adrmode(ticks);				\
  fetch();					\
  A op##= getMemory(ea);			\
  setNZ(A & 0x80, !A);				\
  next();

#define and(ticks, adrmode)	bitwise(ticks, adrmode, &)
#define eor(ticks, adrmode)	bitwise(ticks, adrmode, ^)
#define ora(ticks, adrmode)	bitwise(ticks, adrmode, |)

#define asl(ticks, adrmode)			\
  adrmode(ticks);				\
  {						\
    unsigned int i= getMemory(ea) << 1;		\
    putMemory(ea, i);				\
    fetch();					\
    setNZC(i & 0x80, !i, i >> 8);		\
  }						\
  next();

#define asla(ticks, adrmode)			\
  tick(ticks);					\
  fetch();					\
  {						\
    int c= A >> 7;				\
    A <<= 1;					\
    setNZC(A & 0x80, !A, c);			\
  }						\
  next();

#define lsr(ticks, adrmode)			\
  adrmode(ticks);				\
  {						\
    byte b= getMemory(ea);			\
    int  c= b & 1;				\
    fetch();					\
    b >>= 1;					\
    putMemory(ea, b);				\
    setNZC(0, !b, c);				\
  }						\
  next();

#define lsra(ticks, adrmode)			\
  tick(ticks);					\
  fetch();					\
  {						\
    int c= A & 1;				\
    A >>= 1;					\
    setNZC(0, !A, c);				\
  }						\
  next();

#define rol(ticks, adrmode)			\
  adrmode(ticks);				\
  {						\
    word b= (getMemory(ea) << 1) | getC();	\
    fetch();					\
    putMemory(ea, b);				\
    setNZC(b & 0x80, !(b & 0xFF), b >> 8);	\
  }						\
  next();

#define rola(ticks, adrmode)			\
  tick(ticks);					\
  fetch();					\
  {						\
    word b= (A << 1) | getC();			\
    A= b;					\
    setNZC(A & 0x80, !A, b >> 8);		\
  }						\
  next();

#define ror(ticks, adrmode)			\
  adrmode(ticks);				\
  {						\
    int  c= getC();				\
    byte m= getMemory(ea);			\
    byte b= (c << 7) | (m >> 1);		\
    fetch();					\
    putMemory(ea, b);				\
    setNZC(b & 0x80, !b, m & 1);		\
  }						\
  next();

#define rora(ticks, adrmode)			\
  adrmode(ticks);				\
  {						\
    int ci= getC();				\
    int co= A & 1;				\
    fetch();					\
    A= (ci << 7) | (A >> 1);			\
    setNZC(A & 0x80, !A, co);			\
  }						\
  next();

#define tRS(ticks, adrmode, R, S)		\
  fetch();					\
  tick(ticks);					\
  S= R;						\
  setNZ(S & 0x80, !S);				\
  next();

#define tax(ticks, adrmode)	tRS(ticks, adrmode, A, X)
#define txa(ticks, adrmode)	tRS(ticks, adrmode, X, A)
#define tay(ticks, adrmode)	tRS(ticks, adrmode, A, Y)
#define tya(ticks, adrmode)	tRS(ticks, adrmode, Y, A)
#define tsx(ticks, adrmode)	tRS(ticks, adrmode, S, X)

#define txs(ticks, adrmode)			\
  fetch();					\
  tick(ticks);					\
  S= X;						\
  next();

#define ldR(ticks, adrmode, R)			\
  adrmode(ticks);				\
  fetch();					\
  R= getMemory(ea);				\
  setNZ(R & 0x80, !R);				\
  next();

#define lda(ticks, adrmode)	ldR(ticks, adrmode, A)
#define ldx(ticks, adrmode)	ldR(ticks, adrmode, X)
#define ldy(ticks, adrmode)	ldR(ticks, adrmode, Y)

#define stR(ticks, adrmode, R)			\
  adrmode(ticks);				\
  fetch();					\
  putMemory(ea, R);				\
  next();

#define sta(ticks, adrmode)	stR(ticks, adrmode, A)
#define stx(ticks, adrmode)	stR(ticks, adrmode, X)
#define sty(ticks, adrmode)	stR(ticks, adrmode, Y)
#define stz(ticks, adrmode)	stR(ticks, adrmode, 0)

#define branch(ticks, adrmode, cond)		\
  if (cond)					\
    {						\
      adrmode(ticks);				\
      PC += ea;					\
      tick(1);					\
    }						\
  else						\
    {						\
      tick(ticks);				\
      PC++;					\
    }						\
  fetch();					\
  next();

#define bcc(ticks, adrmode)	branch(ticks, adrmode, !getC())
#define bcs(ticks, adrmode)	branch(ticks, adrmode,  getC())
#define bne(ticks, adrmode)	branch(ticks, adrmode, !getZ())
#define beq(ticks, adrmode)	branch(ticks, adrmode,  getZ())
#define bpl(ticks, adrmode)	branch(ticks, adrmode, !getN())
#define bmi(ticks, adrmode)	branch(ticks, adrmode,  getN())
#define bvc(ticks, adrmode)	branch(ticks, adrmode, !getV())
#define bvs(ticks, adrmode)	branch(ticks, adrmode,  getV())

#define bra(ticks, adrmode)			\
  adrmode(ticks);				\
  PC += ea;					\
  fetch();					\
  tick(1);					\
  next();

#define jmp(ticks, adrmode)				\
  adrmode(ticks);					\
  PC= ea;						\
  if (mpu->callbacks->call[ea])				\
    {							\
      word addr;					\
      externalise();					\
      if ((addr= mpu->callbacks->call[ea](mpu, ea, 0)))	\
	{						\
	  internalise();				\
	  PC= addr;					\
	}						\
    }							\
  fetch();						\
  next();

#define jsr(ticks, adrmode)				\
  PC++;							\
  push(PC >> 8);					\
  push(PC & 0xff);					\
  PC--;							\
  adrmode(ticks);					\
  if (mpu->callbacks->call[ea])				\
    {							\
      word addr;					\
      externalise();					\
      if ((addr= mpu->callbacks->call[ea](mpu, ea, 0)))	\
	{						\
	  internalise();				\
	  PC= addr;					\
	  fetch();					\
	  next();					\
	}						\
    }							\
  PC=ea;						\
  fetch();						\
  next();

#define rts(ticks, adrmode)			\
  tick(ticks);					\
  PC  =  pop();					\
  PC |= (pop() << 8);				\
  PC++;						\
  fetch();					\
  next();

#define brk(ticks, adrmode)					\
  tick(ticks);							\
  PC++;								\
  push(PC >> 8);						\
  push(PC & 0xff);						\
  P |= flagB;							\
  push(P);							\
  P |= flagI;							\
  {								\
    word hdlr= getMemory(0xfffe) + (getMemory(0xffff) << 8);	\
    if (mpu->callbacks->call[hdlr])				\
      {								\
	word addr;						\
	externalise();						\
	if ((addr= mpu->callbacks->call[hdlr](mpu, PC - 2, 0)))	\
	  {							\
	    internalise();					\
	    hdlr= addr;						\
	  }							\
      }								\
    PC= hdlr;							\
  }								\
  fetch();							\
  next();

#define rti(ticks, adrmode)			\
  tick(ticks);					\
  P=     pop();					\
  PC=    pop();					\
  PC |= (pop() << 8);				\
  fetch();					\
  next();

#define nop(ticks, adrmode)			\
  fetch();					\
  tick(ticks);					\
  next();

#define ill(ticks, adrmode)						\
  fetch();								\
  tick(ticks);								\
  fflush(stdout);							\
  fprintf(stderr, "\nundefined instruction %02X\n", memory[PC-1]);	\
  return;

#define phR(ticks, adrmode, R)			\
  fetch();					\
  tick(ticks);					\
  push(R);					\
  next();

#define pha(ticks, adrmode)	phR(ticks, adrmode, A)
#define phx(ticks, adrmode)	phR(ticks, adrmode, X)
#define phy(ticks, adrmode)	phR(ticks, adrmode, Y)
#define php(ticks, adrmode)	phR(ticks, adrmode, P)

#define plR(ticks, adrmode, R)			\
  fetch();					\
  tick(ticks);					\
  R= pop();					\
  setNZ(R & 0x80, !R);				\
  next();

#define pla(ticks, adrmode)	plR(ticks, adrmode, A)
#define plx(ticks, adrmode)	plR(ticks, adrmode, X)
#define ply(ticks, adrmode)	plR(ticks, adrmode, Y)

#define plp(ticks, adrmode)			\
  fetch();					\
  tick(ticks);					\
  P= pop();					\
  next();

#define clF(ticks, adrmode, F)			\
  fetch();					\
  tick(ticks);					\
  P &= ~F;					\
  next();

#define clc(ticks, adrmode)	clF(ticks, adrmode, flagC)
#define cld(ticks, adrmode)	clF(ticks, adrmode, flagD)
#define cli(ticks, adrmode)	clF(ticks, adrmode, flagI)
#define clv(ticks, adrmode)	clF(ticks, adrmode, flagV)

#define seF(ticks, adrmode, F)			\
  fetch();					\
  tick(ticks);					\
  P |= F;					\
  next();

#define sec(ticks, adrmode)	seF(ticks, adrmode, flagC)
#define sed(ticks, adrmode)	seF(ticks, adrmode, flagD)
#define sei(ticks, adrmode)	seF(ticks, adrmode, flagI)

#define do_insns(_)												\
  _(00, brk, implied,   7);  _(01, ora, indx,      6);  _(02, ill, implied,   2);  _(03, ill, implied, 2);      \
  _(04, tsb, zp,        3);  _(05, ora, zp,        3);  _(06, asl, zp,        5);  _(07, ill, implied, 2);      \
  _(08, php, implied,   3);  _(09, ora, immediate, 3);  _(0a, asla,implied,   2);  _(0b, ill, implied, 2);      \
  _(0c, tsb, abs,       4);  _(0d, ora, abs,       4);  _(0e, asl, abs,       6);  _(0f, ill, implied, 2);      \
  _(10, bpl, relative,  2);  _(11, ora, indy,      5);  _(12, ora, indzp,     3);  _(13, ill, implied, 2);      \
  _(14, trb, zp,        3);  _(15, ora, zpx,       4);  _(16, asl, zpx,       6);  _(17, ill, implied, 2);      \
  _(18, clc, implied,   2);  _(19, ora, absy,      4);  _(1a, ina, implied,   2);  _(1b, ill, implied, 2);      \
  _(1c, trb, abs,       4);  _(1d, ora, absx,      4);  _(1e, asl, absx,      7);  _(1f, ill, implied, 2);      \
  _(20, jsr, abs,       6);  _(21, and, indx,      6);  _(22, ill, implied,   2);  _(23, ill, implied, 2);      \
  _(24, bit, zp,        3);  _(25, and, zp,        3);  _(26, rol, zp,        5);  _(27, ill, implied, 2);      \
  _(28, plp, implied,   4);  _(29, and, immediate, 3);  _(2a, rola,implied,   2);  _(2b, ill, implied, 2);      \
  _(2c, bit, abs,       4);  _(2d, and, abs,       4);  _(2e, rol, abs,       6);  _(2f, ill, implied, 2);      \
  _(30, bmi, relative,  2);  _(31, and, indy,      5);  _(32, and, indzp,     3);  _(33, ill, implied, 2);      \
  _(34, bit, zpx,       4);  _(35, and, zpx,       4);  _(36, rol, zpx,       6);  _(37, ill, implied, 2);      \
  _(38, sec, implied,   2);  _(39, and, absy,      4);  _(3a, dea, implied,   2);  _(3b, ill, implied, 2);      \
  _(3c, bit, absx,      4);  _(3d, and, absx,      4);  _(3e, rol, absx,      7);  _(3f, ill, implied, 2);      \
  _(40, rti, implied,   6);  _(41, eor, indx,      6);  _(42, ill, implied,   2);  _(43, ill, implied, 2);      \
  _(44, ill, implied,   2);  _(45, eor, zp,        3);  _(46, lsr, zp,        5);  _(47, ill, implied, 2);      \
  _(48, pha, implied,   3);  _(49, eor, immediate, 3);  _(4a, lsra,implied,   2);  _(4b, ill, implied, 2);      \
  _(4c, jmp, abs,       3);  _(4d, eor, abs,       4);  _(4e, lsr, abs,       6);  _(4f, ill, implied, 2);      \
  _(50, bvc, relative,  2);  _(51, eor, indy,      5);  _(52, eor, indzp,     3);  _(53, ill, implied, 2);      \
  _(54, ill, implied,   2);  _(55, eor, zpx,       4);  _(56, lsr, zpx,       6);  _(57, ill, implied, 2);      \
  _(58, cli, implied,   2);  _(59, eor, absy,      4);  _(5a, phy, implied,   3);  _(5b, ill, implied, 2);      \
  _(5c, ill, implied,   2);  _(5d, eor, absx,      4);  _(5e, lsr, absx,      7);  _(5f, ill, implied, 2);      \
  _(60, rts, implied,   6);  _(61, adc, indx,      6);  _(62, ill, implied,   2);  _(63, ill, implied, 2);      \
  _(64, stz, zp,        3);  _(65, adc, zp,        3);  _(66, ror, zp,        5);  _(67, ill, implied, 2);      \
  _(68, pla, implied,   4);  _(69, adc, immediate, 3);  _(6a, rora,implied,   2);  _(6b, ill, implied, 2);      \
  _(6c, jmp, indirect,  5);  _(6d, adc, abs,       4);  _(6e, ror, abs,       6);  _(6f, ill, implied, 2);      \
  _(70, bvs, relative,  2);  _(71, adc, indy,      5);  _(72, adc, indzp,     3);  _(73, ill, implied, 2);      \
  _(74, stz, zpx,       4);  _(75, adc, zpx,       4);  _(76, ror, zpx,       6);  _(77, ill, implied, 2);      \
  _(78, sei, implied,   2);  _(79, adc, absy,      4);  _(7a, ply, implied,   4);  _(7b, ill, implied, 2);      \
  _(7c, jmp, indabsx,   6);  _(7d, adc, absx,      4);  _(7e, ror, absx,      7);  _(7f, ill, implied, 2);      \
  _(80, bra, relative,  2);  _(81, sta, indx,      6);  _(82, ill, implied,   2);  _(83, ill, implied, 2);      \
  _(84, sty, zp,        2);  _(85, sta, zp,        2);  _(86, stx, zp,        2);  _(87, ill, implied, 2);      \
  _(88, dey, implied,   2);  _(89, bit, immediate, 2);  _(8a, txa, implied,   2);  _(8b, ill, implied, 2);      \
  _(8c, sty, abs,       4);  _(8d, sta, abs,       4);  _(8e, stx, abs,       4);  _(8f, ill, implied, 2);      \
  _(90, bcc, relative,  2);  _(91, sta, indy,      6);  _(92, sta, indzp,     3);  _(93, ill, implied, 2);      \
  _(94, sty, zpx,       4);  _(95, sta, zpx,       4);  _(96, stx, zpy,       4);  _(97, ill, implied, 2);      \
  _(98, tya, implied,   2);  _(99, sta, absy,      5);  _(9a, txs, implied,   2);  _(9b, ill, implied, 2);      \
  _(9c, stz, abs,       4);  _(9d, sta, absx,      5);  _(9e, stz, absx,      5);  _(9f, ill, implied, 2);      \
  _(a0, ldy, immediate, 3);  _(a1, lda, indx,      6);  _(a2, ldx, immediate, 3);  _(a3, ill, implied, 2);      \
  _(a4, ldy, zp,        3);  _(a5, lda, zp,        3);  _(a6, ldx, zp,        3);  _(a7, ill, implied, 2);      \
  _(a8, tay, implied,   2);  _(a9, lda, immediate, 3);  _(aa, tax, implied,   2);  _(ab, ill, implied, 2);      \
  _(ac, ldy, abs,       4);  _(ad, lda, abs,       4);  _(ae, ldx, abs,       4);  _(af, ill, implied, 2);      \
  _(b0, bcs, relative,  2);  _(b1, lda, indy,      5);  _(b2, lda, indzp,     3);  _(b3, ill, implied, 2);      \
  _(b4, ldy, zpx,       4);  _(b5, lda, zpx,       4);  _(b6, ldx, zpy,       4);  _(b7, ill, implied, 2);      \
  _(b8, clv, implied,   2);  _(b9, lda, absy,      4);  _(ba, tsx, implied,   2);  _(bb, ill, implied, 2);      \
  _(bc, ldy, absx,      4);  _(bd, lda, absx,      4);  _(be, ldx, absy,      4);  _(bf, ill, implied, 2);      \
  _(c0, cpy, immediate, 3);  _(c1, cmp, indx,      6);  _(c2, ill, implied,   2);  _(c3, ill, implied, 2);      \
  _(c4, cpy, zp,        3);  _(c5, cmp, zp,        3);  _(c6, dec, zp,        5);  _(c7, ill, implied, 2);      \
  _(c8, iny, implied,   2);  _(c9, cmp, immediate, 3);  _(ca, dex, implied,   2);  _(cb, ill, implied, 2);      \
  _(cc, cpy, abs,       4);  _(cd, cmp, abs,       4);  _(ce, dec, abs,       6);  _(cf, ill, implied, 2);      \
  _(d0, bne, relative,  2);  _(d1, cmp, indy,      5);  _(d2, cmp, indzp,     3);  _(d3, ill, implied, 2);      \
  _(d4, ill, implied,   2);  _(d5, cmp, zpx,       4);  _(d6, dec, zpx,       6);  _(d7, ill, implied, 2);      \
  _(d8, cld, implied,   2);  _(d9, cmp, absy,      4);  _(da, phx, implied,   3);  _(db, ill, implied, 2);      \
  _(dc, ill, implied,   2);  _(dd, cmp, absx,      4);  _(de, dec, absx,      7);  _(df, ill, implied, 2);      \
  _(e0, cpx, immediate, 3);  _(e1, sbc, indx,      6);  _(e2, ill, implied,   2);  _(e3, ill, implied, 2);      \
  _(e4, cpx, zp,        3);  _(e5, sbc, zp,        3);  _(e6, inc, zp,        5);  _(e7, ill, implied, 2);      \
  _(e8, inx, implied,   2);  _(e9, sbc, immediate, 3);  _(ea, nop, implied,   2);  _(eb, ill, implied, 2);      \
  _(ec, cpx, abs,       4);  _(ed, sbc, abs,       4);  _(ee, inc, abs,       6);  _(ef, ill, implied, 2);      \
  _(f0, beq, relative,  2);  _(f1, sbc, indy,      5);  _(f2, sbc, indzp,     3);  _(f3, ill, implied, 2);      \
  _(f4, ill, implied,   2);  _(f5, sbc, zpx,       4);  _(f6, inc, zpx,       6);  _(f7, ill, implied, 2);      \
  _(f8, sed, implied,   2);  _(f9, sbc, absy,      4);  _(fa, plx, implied,   4);  _(fb, ill, implied, 2);      \
  _(fc, ill, implied,   2);  _(fd, sbc, absx,      4);  _(fe, inc, absx,      7);  _(ff, ill, implied, 2);






#include "lib6502_dump.c"
#include "lib6502_main.c"

