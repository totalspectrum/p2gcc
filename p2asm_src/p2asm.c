#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include <ctype.h>
#include "strsubs.h"
#include "symsubs.h"

// Printing modes
#define PRINT_NOCODE    0
#define PRINT_CODE      1
#define PRINT_NONE      2

// Spin section types
#define MODE_DAT 0
#define MODE_CON 1

// C, Z and I bits
#define C_BIT (1 << 20)
#define Z_BIT (1 << 19)
#define I_BIT (1 << 18)

#define DATA_BUFFER_SIZE 1000000

// Keywords and delimiters
char *SectionKeywords[] = {"dat", "con", "pub", "pri", "var", 0};

FILE *infile;
FILE *lstfile;
FILE *binfile;
FILE *objfile;

SymbolT SymbolTable[MAX_SYMBOLS];
char databuffer[DATA_BUFFER_SIZE];

int numsym = 0;
int cog_addr = 0;
int hub_addr = 0;
int printlevel = 3;
int unicode = 0;
int hubmode = 0;
int bincount = 0;
int objflag = 0;
int hasmain = 0;
char buffer2[300];
int debugflag = 0;
int pictableaddr = 0x7fffffff;
int pictablemode = 0;
char PicTableName[1000][MAX_SYMBOL_LEN+1];
int pictablenum = 0;
int case_sensative = 0;
int picflag = 0;
int textmode = 1;
int hubonly = 0;

void DumpIt(int printflag, void *ptr, int num);
void PrintIt(int printflag, int hub_addr, int cog_addr, int data_size, char *buffer2, void *ptr);

void PrintError(char *str, ...)
{
    va_list ap;
    va_start(ap, str);
    vprintf(str, ap);
    vfprintf(lstfile, str, ap);
    va_end(ap);
    printf("%s\n", buffer2);
}

void PrintUnexpected(int num, char *str)
{
    PrintError("ERROR: (%d) Unexpected symbol \"%s\"\n", num, str);
}

int CheckExpected(char *ptr1, int i, char **tokens, int num)
{
    if (i >= num)
    {
        PrintError("ERROR: Expected \"%s\", but found EOL\n", ptr1, 0);
        return 1;
    }
    if (strcmp(ptr1, tokens[i]))
    {
        PrintError("ERROR: Expected \"%s\", but found \"%s\"\n", ptr1, tokens[i]);
        return 1;
    }
    return 0;
}

int CheckForEOL(int i, int num)
{
    if (i >= num)
    {
        PrintError("ERROR: Unexpected EOL\n", 0, 0);
        return 1;
    }
    return 0;
}

// This routine handles the P2 pointer syntax
int EncodePointerField(int *pindex, char **tokens, int num)
{
    int value;
    int negate = 0;
    int i = *pindex;
    int is_float = -1;
    int retval = 0x100;

    // Check for pre-increment/decrement
    if (!strcmp(tokens[i], "++"))
    {
        retval |= 0x41;
        if (++i >= num) { *pindex = i; return retval; }
    }
    else if (!strcmp(tokens[i], "--"))
    {
        negate = 1;
        retval |= 0x5f;
        if (++i >= num) { *pindex = i; return retval; }
    }

    value = StrCompNoCase(tokens[i], "ptrb");
    retval |= (value & 1) << 7;
    if (i >= num - 1) { *pindex = i; return retval; }

    // Check for post-increment/decrement
    if (!strcmp(tokens[i+1], "++"))
    {
        retval |= 0x61;
        if (++i >= num - 1) { *pindex = i; return retval; }
    }
    else if (!strcmp(tokens[i+1], "--"))
    {
        negate = 1;
        retval |= 0x7f;
        if (++i >= num - 1) { *pindex = i; return retval; }
    }

    // Check for opening bracket
    if (strcmp(tokens[i+1], "[")) { *pindex = i; return retval; }

    if (++i > num - 1)
    {
	*pindex = i;
	return -1;
    }

    i++;
    EvaluateExpression(12, &i, tokens, num, &value, &is_float);
    if (negate) value = -value;

    if (++i >= num)
    {
	*pindex = i;
	return -1;
    }

    if (value < -16 || value > 15)
    {
        fprintf(lstfile, "Warning: pointer index %d is out of bounds\n", value);
    }

    retval = (retval & ~31) | (value & 31);

    if (i >= num)
    {
        *pindex = i;
	return -1;
    }

    if (CheckExpected("]", i, tokens, num))
    {
        *pindex = i;
        return -1;
    }

    *pindex = i;
    return retval;
}

void GenerateAugx(int opcode, int value, int dfield)
{
    opcode &= 0xf0000000; // Extract the condition code
    if (dfield)
        opcode |= 0x0f800000; // AUGD
    else
        opcode |= 0x0f000000; // AUGS
    opcode |= (value >> 9) & 0x7fffff;
    PrintIt(PRINT_CODE, hub_addr, cog_addr, 4, buffer2, &opcode);
    DumpIt(PRINT_CODE, &opcode, 4);
    buffer2[0] = 0;
    cog_addr += 4;
    hub_addr += 4;
}

// This routine processes the source or destination field
// P2 pointer expressions are allowed if type is >= 2
// Immediate values using a # are allowed if type is >= 1
// The value is returned if successful, and bit at 0x200 is set if immediate
// A minus one is return if unsuccessful
int EncodeAddressField(int *pindex, char **tokens, int num, int type, int opcode, int dfield)
{
    int retval = 0;
    int i = *pindex;
    int value, errnum;
    char *name = tokens[i];
    int extended = 0;
    int is_float = -1;

    if (StrCompNoCase(name, "ptra") || StrCompNoCase(name, "ptrb") ||
        !strcmp(name, "++") || !strcmp(name, "--"))
    {
	if (type >= 2)
	{
            value = EncodePointerField(&i, tokens, num);
	    if (value >= 0) value |= 0x200;
	}
	else
	{
	    PrintError("ERROR: PTR not allowed\n", 0, 0);
	    value = -1;
	}
	*pindex = i;
	return value;
    }

    if (!strcmp(name, "#") || !strcmp(name, "##"))
    {
        if (!strcmp(name, "##")) extended = 1;
	if (type < 1)
	{
            PrintError("ERROR: Bad symbol type - %d\n", type, 0);
	    *pindex = i;
	    return -1;
	}
	retval = 0x200;
        if (CheckForEOL(++i, num))
	{
	    *pindex = i;
	    return -1;
	}
        errnum = EvaluateExpression(12, &i, tokens, num, &value, &is_float);
        if (extended) GenerateAugx(opcode, value, dfield);
    }
    else
    {
        errnum = EvaluateExpression(12, &i, tokens, num, &value, &is_float);
    }

    if (errnum)
    {
        *pindex = i;
        return -1;
    }
    retval |= (value & 511);
    *pindex = i;
    return retval;
}

void ExpectDone(int *pindex, char **tokens, int num)
{
    if (*pindex < num - 1) PrintUnexpected(8, tokens[*pindex]);
    *pindex = num - 1;
}

// Handle conditional flags wc, wz and wcz
int ProcessWx(int *pi, char **tokens, int num, int *popcode)
{
    int index;
    SymbolT *s;
    int i = *pi + 1;

    if (i >= num) { *pi = i; return 0; }

    index = FindSymbol(tokens[i]);
    if (index < 0) { *pi = i; return 1; }
    s = &SymbolTable[index];

    if (s->type != TYPE_WX)
    {
        PrintUnexpected(1, tokens[i]);
        *pi = i;
        return 1;
    }
    *popcode |= s->value;
    if (++i < num)
    {
        PrintUnexpected(2, tokens[i]);
        *pi = i;
        return 1;
    }

    *pi = i;

    return 0;
}

// Handle conditional flags wc, wz, andc, andz, orc, orz, xorc and xorz
int ProcessWlx(int *pi, char **tokens, int num, int *popcode, int shift)
{
    int index;
    SymbolT *s;
    int i = *pi + 1;

    if (CheckForEOL(i, num))
    {
        *pi = i;
        return 1;
    }

    index = FindSymbol(tokens[i]);
    if (index < 0) { *pi = i; return 1; }
    s = &SymbolTable[index];

    if ((s->type != TYPE_WX && s->type != TYPE_WLX) || !strcmp(tokens[i], "wcz"))
    {
        PrintUnexpected(3, tokens[i]);
        *pi = i;
        return 1;
    }
    *popcode |= s->value & ~3;
    *popcode |= (s->value & 3) << shift;
    if (++i < num)
    {
        PrintUnexpected(4, tokens[i]);
        *pi = i;
        return 1;
    }

    *pi = i;

    return 0;
}

// Handle conditional flag wcz
int ProcessWcz(int *pi, char **tokens, int num, int *popcode)
{
    int index;
    SymbolT *s;
    int i = *pi + 1;

    if (i >= num)
    {
        *pi = i;
        return 0;
    }

    index = FindSymbol(tokens[i]);
    if (index < 0) { *pi = i; return 1; }
    s = &SymbolTable[index];

    if (strcmp(tokens[i], "wcz"))
    {
        PrintUnexpected(5, tokens[i]);
        *pi = i;
        return 1;
    }
    *popcode |= s->value;
    if (++i < num)
    {
        PrintUnexpected(6, tokens[i]);
        *pi = i;
        return 1;
    }

    *pi = i;

    return 0;
}

void WriteObjectEntry(int type, int addr, char *str)
{
    int len = strlen(str) + 1;
    fwrite(&type, 1, 1, objfile);
    fwrite(&addr, 1, 4, objfile);
    fwrite(&len, 1, 1, objfile);
    fwrite(str, 1, len, objfile);
}

// Handle large address fields
int ProcessBigSrc(int *pi, char **tokens, int num, int *popcode)
{
    int value;
    int rflag = 0;
    int forceabs = 0;
    int is_float = -1;
    int target_hubmode;

//printf("Trace 5\n");
    (*pi)++;
    if (!strcmp(tokens[*pi], "\\"))
    {
//printf("Force Absolute - %d\n", *pi);
        (*pi)++;
        forceabs = 1;
//printf("*pi is now %d, token is %s\n", *pi, tokens[*pi]);
    }
    if (objflag && *pi == num - 1)
    {
        int index = FindSymbol(tokens[*pi]);
        if (index < 0)
        {
            value = hub_addr + 4;
            if (debugflag) printf("UNDEFI %8.8x %s\n", hub_addr, tokens[*pi]);
            WriteObjectEntry('U', hub_addr, tokens[*pi]);
        }
        else if (SymbolTable[index].type == TYPE_HUB_ADDR)
            value = SymbolTable[index].value2;
        else
            value = SymbolTable[index].value;
    }
    else if (EvaluateExpression(12, pi, tokens, num, &value, &is_float)) return 1;
    target_hubmode = value >= 0x400;
    if (hubmode == target_hubmode && !forceabs) rflag = 1;
    if (rflag)
    {
        *popcode |= C_BIT;
        if (hubmode)
            value -= hub_addr + 4;
        else
            value = (value << 2) - cog_addr - 4;
    }
    *popcode |= (value & 0xfffff);
    return ProcessWx(pi, tokens, num, popcode);
}

int GetImmSrcValue(int i, char **tokens, int num, int *retval)
{
    int value;
    int rflag = 0;
    int is_float = -1;
    int target_hubmode;

    if (!strcmp(tokens[++i], "\\")) i++;
//printf("Trace 4 - %s, %d, %d\n", tokens[i], i, num);
    if (objflag && i == num - 1 && FindSymbol(tokens[i]) < 0)
        value = 0x1000000;
    else if (EvaluateExpression(12, &i, tokens, num, &value, &is_float)) return 1;
    target_hubmode = value >= 0x400;
    if (hubmode == target_hubmode) rflag = 1;
    if (rflag)
    {
        if (hubmode)
            value -= hub_addr + 4;
        else
            value = (value << 2) - cog_addr - 4;
    }
    *retval = value;
    return 0;
}

int CountCommas(int i, char **tokens, int num)
{
    int count = 0;

    while (i < num)
    {
        if (!strcmp(tokens[i++], ",")) count++;
    }

    return count;
}

int CountExtras(int i, char **tokens, int num)
{
    int value;
    int count = 0;
    int is_float = -1;

    while (i < num)
    {
        //if (!strcmp(tokens[i++], "[")) count += atol(tokens[i]) - 1;
        if (!strcmp(tokens[i++], "["))
        {
            if (EvaluateExpression(12, &i, tokens, num, &value, &is_float)) break;
            count += value - 1;
            i++;
        }
    }

    return count;
}

int CountStrings(int i, char **tokens, int num)
{
    int count = 0;

    for (;i < num; i++)
    {
        if (tokens[i][0] == '"') count += strlen(tokens[i]) - 3;
    }

    return count;
}

int CountElements(int i, char **tokens, int num)
{
    int num_elem = 1;

    if (i >= num) return 0;    
    num_elem += CountCommas(i, tokens, num);
    num_elem += CountExtras(i, tokens, num);
    num_elem += CountStrings(i, tokens, num);
    return num_elem;
}

void DumpIt(int printflag, void *ptr, int num)
{
    if (printflag != PRINT_NOCODE)
    {
        fwrite(ptr, 1, num, binfile);
        bincount += num;
    }
}

int GetData(int i, char **tokens, int num, int datasize)
{
    int count;
    int j = 0;
    int value = 0;
    int is_float = -1;
    int hub_addr0 = hub_addr;
    int cog_addr0 = cog_addr;
    for (;i < num; i++)
    {
        if (tokens[i][0] == '{') continue;
        if (!strcmp(tokens[i], ",")) continue;
        if (tokens[i][0] == '"')
        {
            char *ptr = tokens[i] + 1;
            while (*ptr && *ptr != '"')
            {
                if (j + datasize > DATA_BUFFER_SIZE)
                {
                    PrintError("ERROR: Data buffer overflow\n", 0, 0);
                    hub_addr = hub_addr0;
                    cog_addr = cog_addr0;
                    return j;
                }
                value = *ptr++;
                memcpy(&databuffer[j], &value, datasize);
                j += datasize;
                hub_addr += datasize;
                cog_addr += datasize;
            }
            continue;
        }
        if (!strcmp(tokens[i], "["))
        {
            i++;
            if (EvaluateExpression(12, &i, tokens, num, &count, &is_float)) break;
            i++;
            count--;
        }
        else
        {
            count = 1;
            if (objflag && datasize == 4 && (i == num - 1 || !strcmp(tokens[i+1], ",")))
            {
                int index = FindSymbol(tokens[i]);
                if (index < 0)
                {
                    if (hub_addr/*+j*/ >= 0x400 || !hubonly)
                    {
                        value = 0;
                        if (debugflag) printf("UNDEFI %8.8x %s\n", hub_addr/*+j*/, tokens[i]);
                        WriteObjectEntry('u', hub_addr/*+j*/, tokens[i]);
                    }
                }
                else if (SymbolTable[index].type == TYPE_HUB_ADDR)
                {
                    if (hub_addr/*+j*/ >= 0x400 || !hubonly)
                    {
                        value = SymbolTable[index].value2;
                        if (debugflag) printf("VREF %8.8x %s\n", hub_addr/*+j*/, tokens[i]);
                        WriteObjectEntry('R', hub_addr/*+j*/, tokens[i]);
                    }
                }
                else
                    value = SymbolTable[index].value;
            }
            else
            {
                int i0 = i;
                int index = FindSymbol(tokens[i0]);
                if (EvaluateExpression(12, &i, tokens, num, &value, &is_float)) break;
                if (objflag && datasize == 4 && index >= 0 &&
                    (i0 == num - 3 || !strcmp(tokens[i0+3], ",")) &&
                    SymbolTable[index].type == TYPE_HUB_ADDR &&
                    (hub_addr >= 0x400 || !hubonly))
                {
                    if (debugflag) printf("VREF %8.8x %s\n", hub_addr, tokens[i0]);
                    WriteObjectEntry('R', hub_addr, tokens[i0]);
                    if (debugflag)
                    {
                        printf("Found Offset Reference -");
                        for (;i0 <= i; i0++) printf(" %s", tokens[i0]);
                        printf("\n");
                    }
                }
            }
        }
        while (count-- > 0)
        {
            if (j + datasize > DATA_BUFFER_SIZE)
            {
                PrintError("ERROR: Data buffer overflow\n", 0, 0);
                hub_addr = hub_addr0;
                cog_addr = cog_addr0;
                return j;
            }
            memcpy(&databuffer[j], &value, datasize);
            hub_addr += datasize;
            cog_addr += datasize;
            j += datasize;
        } 
    }
    hub_addr = hub_addr0;
    cog_addr = cog_addr0;
    return j;
}

// Handle source for instructions such as djnz d,s/#
int ProcessRsrc(int *pi, char **tokens, int num, int *popcode)
{
    int value;
    int i = *pi;
    int rflag = 0;
    int is_float = -1;

    i++;

//printf("Trace 6\n");
    if (!strcmp(tokens[i], "#"))
    {
        i++;
        rflag = 1;
        *popcode |= I_BIT;
    }

    if (EvaluateExpression(12, &i, tokens, num, &value, &is_float))
    {
        *pi = i;
        return 1;
    }

    if (rflag)
    {
        if (hubmode)
        {
            value -= hub_addr + 4;
            value >>= 2;
        }
        else
            value -= (cog_addr >> 2) + 1;
    }
    *popcode |= (value & 511);

    *pi = i;
    return ProcessWx(pi, tokens, num, popcode);
}

void CheckVref(int i, char **tokens, int num, int srcflag)
{
    int index;
    SymbolT *s;

    if (!objflag || picflag) return;
    if (strcmp(tokens[i++], "##")) return;
    //printf("CheckVref: %s\n", tokens[i]);
    index = FindSymbol(tokens[i]);
    if (index < 0) return;
    s = &SymbolTable[index];
    if (s->type != TYPE_HUB_ADDR) return;
    if (debugflag) printf("VREF %8.8x %s\n", hub_addr, s->name);
    if (srcflag)
        WriteObjectEntry('W', hub_addr, s->name);
    else
        WriteObjectEntry('X', hub_addr, s->name);
}

// Handle source for OP1S or OP2 instruction, such as jmp s or add d,s/#n
int ProcessSrc(int *pi, char **tokens, int num, int *popcode)
{
    int value;
    char *name;
    int extended = 0;
    int is_float = -1;
    int immediate = 0;

    (*pi)++;
    CheckVref(*pi, tokens, num, 1);
    name = tokens[*pi];
    if (!strcmp(name, "#") || !strcmp(name, "##"))
    {
        (*pi)++;
        *popcode |= I_BIT;
        extended = !strcmp(name, "##");
        immediate = 1;
    }
    if (EvaluateExpression(12, pi, tokens, num, &value, &is_float)) return 1;
    if (extended) GenerateAugx(*popcode, value, 0);
    if (objflag && picflag)
    {
        if (!immediate && value >= pictableaddr && value < 0x1f0 && hub_addr >= 0x400)
        {
            if (debugflag) printf("VREF %8.8x %s\n", hub_addr, PicTableName[value-pictableaddr]);
            WriteObjectEntry('W', hub_addr, PicTableName[value-pictableaddr]);
        }
    }
    *popcode |= (value & 511);
    return ProcessWx(pi, tokens, num, popcode);
}

// Handle source for TESTB instruction, such as testb d,s/# wc
int ProcessSrcWlx(int *pi, char **tokens, int num, int *popcode)
{
    int value;
    char *name;
    int extended = 0;
    int is_float = -1;
    int immediate = 0;

    (*pi)++;
    CheckVref(*pi, tokens, num, 1);
    name = tokens[*pi];
    if (!strcmp(name, "#") || !strcmp(name, "##"))
    {
        (*pi)++;
        *popcode |= I_BIT;
        extended = !strcmp(name, "##");
        immediate = 1;
    }
    if (EvaluateExpression(12, pi, tokens, num, &value, &is_float)) return 1;
    if (extended) GenerateAugx(*popcode, value, 0);
    if (objflag && picflag)
    {
        if (!immediate && value >= pictableaddr && value < 0x1f0 && hub_addr >= 0x400)
        {
            if (debugflag) printf("VREF %8.8x %s\n", hub_addr, PicTableName[value-pictableaddr]);
            WriteObjectEntry('W', hub_addr, PicTableName[value-pictableaddr]);
        }
    }
    *popcode |= (value & 511);
    return ProcessWlx(pi, tokens, num, popcode, 22);
}

// Handle source for BITL instruction, such as bitl d,s/# wcz
int ProcessSrcWcz(int *pi, char **tokens, int num, int *popcode)
{
    int value;
    char *name;
    int extended = 0;
    int is_float = -1;
    int immediate = 0;

    (*pi)++;
    CheckVref(*pi, tokens, num, 1);
    name = tokens[*pi];
    if (!strcmp(name, "#") || !strcmp(name, "##"))
    {
        (*pi)++;
        *popcode |= I_BIT;
        extended = !strcmp(name, "##");
        immediate = 1;
    }
    if (EvaluateExpression(12, pi, tokens, num, &value, &is_float)) return 1;
    if (extended) GenerateAugx(*popcode, value, 0);
    if (objflag && picflag)
    {
        if (!immediate && value >= pictableaddr && value < 0x1f0 && hub_addr >= 0x400)
        {
            if (debugflag) printf("VREF %8.8x %s\n", hub_addr, PicTableName[value-pictableaddr]);
            WriteObjectEntry('W', hub_addr, PicTableName[value-pictableaddr]);
        }
    }
    *popcode |= (value & 511);
    return ProcessWcz(pi, tokens, num, popcode);
}

// Handle source rdxxxx and wrxxxx
int ProcessPointerSrc(int *pi, char **tokens, int num, int *popcode)
{
    int value;
    int i = (*pi) + 1;

    if (CheckForEOL(*pi = i, num)) return 1;
    CheckVref(i, tokens, num, 1);
    value = EncodeAddressField(pi, tokens, num, 2, *popcode, 0);
    if (value < 0) return 1;
    if (value & 0x200) *popcode |= I_BIT;
    else if (value >= pictableaddr && value < 0x1f0 && hub_addr >= 0x400)
    {
        if (debugflag) printf("VREF %8.8x %s\n", hub_addr, PicTableName[value-pictableaddr]);
        WriteObjectEntry('W', hub_addr, PicTableName[value-pictableaddr]);
    }
    *popcode |= (value & 0x1ff);
    return ProcessWx(pi, tokens, num, popcode);
}

int FindNeededSymbol(char *str, int pass)
{
    int opcode = 0;
    int index = FindSymbol(str);
    if (index < 0)
    {
        PrintError("ERROR: %s is undefined\n", str, 0);
        if (pass == 2)
            PrintIt(PRINT_NOCODE, hub_addr, cog_addr, 0, buffer2, &opcode);
    }
    return index;
}

int GetModczParm(int *pi, char **tokens, int num)
{
    int index;
    SymbolT *s;
    int i = *pi;

    if (CheckForEOL(i, num)) return 0;
    *pi = i + 1;
    if (!strcmp(tokens[i], "0")) return 0;
    index = FindNeededSymbol(tokens[i], 2);
    if (index < 0) return 0;
    s = &SymbolTable[index];
    if (s->type != TYPE_MODCZP)
    {
        PrintError("ERROR: Expected MODCZ parameter - %s\n", tokens[i]);
        return 0;
    }
    return s->value;
}

// Parse a line in the DAT section
void ParseDat(int pass, char *buffer2, char **tokens, int num)
{
    int i = 0;
    SymbolT *s;
    int index;
    int value;
    int opindex = 0;
    int is_float = -1;
    int data_size = 4;
    int cog_incr = 0;
    int hub_incr = 0;
    int printflag = PRINT_CODE;
    int opcode = 0xf0000000; // Initialize to if_always
    int optype;
    int datalen;

    if (num < 1)
    {
        printf("ParseDat: num = %d\n", num);
        return;
    }

    // Check the first token for a label
    if (StrCompNoCase(tokens[i], "dat"))
        i++;
    else
    {
        // Find the token is in the symbol table
        index = FindSymbol(tokens[i]);

        // Check for an undefined symbol or local label
        if (index < 0 || (pass == 1 && tokens[i][0] == '.'))
        {
            AddSymbol2(tokens[i], cog_addr >> 2, hub_addr, hubmode ? TYPE_HUB_ADDR : TYPE_COG_ADDR);
            if (!strcmp(tokens[i], "pictable"))
            {
                pictableaddr = cog_addr >> 2;
                if (debugflag) printf("PICTABLE %8.8x %s\n", hub_addr, "pictable");
                WriteObjectEntry('P', hub_addr, "pictable");
            }
            if (!strcmp(tokens[i], "_hub_pic_table")) pictablemode = 1;
            if (!strcmp(tokens[i], "_hub_pic_count")) pictablemode = 0;
            if (num == 1) printflag = PRINT_NOCODE;
            i++;
        }
        else
        {
            // Get a pointer to the symbol in the table
            s = &SymbolTable[index];

            // Check if this is an address label
            if (s->type == TYPE_HUB_ADDR || s->type == TYPE_COG_ADDR)
            {
                if (pass == 2)
                {
                    if (tokens[i][0] != '.') PurgeLocalLabels(index);
                    if (num == 1) printflag = PRINT_NOCODE;
                }
                else
                {
                    // If this is pass 1 then this label is already defined
                    printf("label %s is already defined\n", tokens[i]);
                }
                i++;
            }
        }
    }

    if (i >= num)
    {
        if (pass == 2)
            PrintIt(PRINT_NOCODE, hub_addr, cog_addr, data_size, buffer2, &opcode);
        return;
    }

    if ((index = FindNeededSymbol(tokens[i], pass)) < 0) return;

    // Check for a conditional
    if (SymbolTable[index].type == TYPE_IF)
    {
        opcode = SymbolTable[index].value;
        cog_incr = 4;
        hub_incr = 4;
        i++;

        if (CheckForEOL(i, num))
        {
            if (pass == 2) PrintIt(PRINT_NOCODE, hub_addr, cog_addr, data_size, buffer2, &opcode);
            return;
        }

        if ((index = FindNeededSymbol(tokens[i], pass)) < 0) return;
    }

    // Get a pointer to the symbol in the table
    s = &SymbolTable[index];
    optype = s->type;
    i++;
    // Check for NULL value and type, which is a NOP
    if (s->value || s->type)
        opcode |= s->value;
    else
        opcode = 0;

    opindex = index;

    // Check for opcode or psuedo-op
    cog_incr = 4;
    hub_incr = 4;

    // Just count opcodes if pass 1 and token is a normal instruction
    if (pass == 1 && (optype <= 3 || (optype >= 23 && optype <= 51)))
    {
        cog_addr += 4;
        hub_addr += 4;
        while (i < num)
        {
            if (!strcmp(tokens[i++], "##"))
            {
                cog_addr += 4;
                hub_addr += 4;
            }
        }
        return;
    }

    switch (optype)
    {
        case TYPE_ORG:
        {
            hubmode = 0;
            printflag = PRINT_NOCODE;
            hub_addr = (hub_addr + 3) & ~3;
            if (i == num)
                cog_addr = 0;
            else
            {
                if (EvaluateExpression(12, &i, tokens, num, &cog_addr, &is_float)) break;
                cog_addr <<= 2;
            }
            cog_incr = 0;
            hub_incr = 0;
            break;
        }

        case TYPE_ORGF:
        {
            hubmode = 0;
            printflag = PRINT_NOCODE;
            hub_addr = (hub_addr + 3) & ~3;
            if (i == num)
                cog_addr = 0;
            else
            {
                int prev_cog_addr = cog_addr;
                if (EvaluateExpression(12, &i, tokens, num, &cog_addr, &is_float)) break;
                cog_addr <<= 2;
                if (cog_addr > prev_cog_addr)
                {
                    hub_addr += cog_addr - prev_cog_addr;
                    if (pass == 2)
                    {
                        int value = 0;
                        for (; prev_cog_addr < cog_addr; prev_cog_addr++) DumpIt(PRINT_CODE, &value, 1);
                    }
                }
            }
            cog_incr = 0;
            hub_incr = 0;
            break;
        }

        case TYPE_ORGH:
        {
            int new_hub_addr = hub_addr;
            hubmode = 1;
            printflag = PRINT_NOCODE;
            if (i < num)
            {
                if (EvaluateExpression(12, &i, tokens, num, &new_hub_addr, &is_float)) break;
                if (new_hub_addr < hub_addr)
                    PrintError("ERROR: ORGH address %x less than previous address %x\n", new_hub_addr, hub_addr);
                if (pass == 2)
                {
                    int value = 0;
                    for (; hub_addr < new_hub_addr; hub_addr++) DumpIt(PRINT_CODE, &value, 1);
                }
                hub_addr = new_hub_addr;
            }
            cog_addr = 0;
            cog_incr = 0;
            hub_incr = 0;
            break;
        }

        case TYPE_ALIGNL:
        {
            int value = 0;
            hub_incr = (4 - hub_addr) & 3;
            printflag = PRINT_NOCODE;
            if (hub_incr && pass == 2) DumpIt(PRINT_CODE, &value, hub_incr);
            ExpectDone(&i, tokens, num);
            break;
        }

        case TYPE_ALIGNW:
        {
            int value = 0;
            hub_incr = hub_addr & 1;
            printflag = PRINT_NOCODE;
            if (hub_incr && pass == 2) DumpIt(PRINT_CODE, &value, hub_incr);
            ExpectDone(&i, tokens, num);
            break;
        }

        case TYPE_TEXT:
        {
            textmode = 1;
            return;
        }

        case TYPE_DATA:
        {
            textmode = 0;
            return;
        }

        case TYPE_GLOBAL:
        {
            if (pass == 2 && objflag)
            {
                if (debugflag) printf("GLOBAL %8.8x %s\n", hub_addr, tokens[0]);
                if (textmode)
                    WriteObjectEntry('G', hub_addr, tokens[0]);
                else
                    WriteObjectEntry('D', hub_addr, tokens[0]);
            }
            return;
        }

        case TYPE_GLOBAL0:
        {
            if (pass == 2 && objflag)
            {
                if (debugflag) printf("GLOBAL0 %8.8x %s\n", hub_addr, tokens[0]);
                WriteObjectEntry('d', hub_addr, tokens[0]);
            }
            return;
        }

        case TYPE_RES:
        {
            hub_incr = 0;
            printflag = PRINT_NOCODE;
            if (i == num)
                cog_incr = 4;
            else
            {
                if (EvaluateExpression(12, &i, tokens, num, &cog_incr, &is_float)) break;
                cog_incr <<= 2;
            }
            cog_addr = (cog_addr + 3) & ~3;
            hub_addr = (hub_addr + 3) & ~3;
            break;
        }

        case TYPE_FIT:
        {
            printflag = PRINT_NOCODE;
            cog_incr = 0;
            hub_incr = 0;
            break;
        }

        case TYPE_LONG:
        {
            cog_incr = hub_incr = 4 * CountElements(i, tokens, num);
            if (pass == 2)
            {
                datalen = GetData(i, tokens, num, 4);
                if (datalen) DumpIt(printflag, databuffer, datalen);
                PrintIt(printflag, hub_addr, cog_addr, datalen, buffer2, databuffer);
            }
            else if (pictablemode)
            {
                //PicTableValue[pictablenum] = hub_addr;
                if (pictablemode == 2)
                    strcpy(PicTableName[pictablenum++], tokens[1]);
                else
                    pictablemode = 2;
            }
            cog_addr += cog_incr;
            hub_addr += hub_incr;
            return;
        }

        case TYPE_WORD:
        {
            cog_incr = hub_incr = 2 * CountElements(i, tokens, num);
            if (pass == 2)
            {
                datalen = GetData(i, tokens, num, 2);
                if (datalen) DumpIt(printflag, databuffer, datalen);
                PrintIt(printflag, hub_addr, cog_addr, datalen, buffer2, databuffer);
            }
            cog_addr += cog_incr;
            hub_addr += hub_incr;
            return;
        }

        case TYPE_BYTE:
        {
            cog_incr = hub_incr = CountElements(i, tokens, num);
            if (pass == 2)
            {
                datalen = GetData(i, tokens, num, 1);
                if (datalen) DumpIt(printflag, databuffer, datalen);
                PrintIt(printflag, hub_addr, cog_addr, datalen, buffer2, databuffer);
            }
            cog_addr += cog_incr;
            hub_addr += hub_incr;
            return;
        }
        case TYPE_FILE:
        {
            char filename[100];
            int len, filesize = 0;
            FILE *tempfile;
            strcpy(filename, &tokens[i][1]);
            len = strlen(filename);
            if (len) filename[len-1] = 0;
            tempfile = fopen(filename, "rb");
            if (!tempfile)
            {
                PrintError("ERROR: Couldn't open %s\n", filename, 0);
            }
            else
            {
                fseek(tempfile, 0, SEEK_END);
                filesize = ftell(tempfile);
                fclose(tempfile);
            }
            cog_incr = hub_incr = filesize;
            if (pass == 2)
            {
                fprintf(lstfile, "%5.5x %3.3x          %s\n", hub_addr, cog_addr >> 2, buffer2);
                tempfile = fopen(filename, "rb");
                if (tempfile)
                {
                    while (1)
                    {
                        len = fread(databuffer, 1, 1000, tempfile);
                        if (len <= 0) break;
                        fwrite(databuffer, 1, len, binfile);
                        bincount += len;
                    }
                }
            }
            cog_addr += cog_incr;
            hub_addr += hub_incr;
            return;
        }

        // Handle OP2 instruction, such as add d,s
        case TYPE_OP2:
        {
            if (EvaluateExpression(12, &i, tokens, num, &value, &is_float)) break;
	    opcode |= (value & 511) << 9;
	    i++;
            if (CheckExpected(",", i, tokens, num)) break;
            ProcessSrc(&i, tokens, num, &opcode);
            break;
        }

        case TYPE_RCZR:
        {
            if (EvaluateExpression(12, &i, tokens, num, &value, &is_float)) break;
	    opcode |= (value & 511) << 9;
            ProcessWx(&i, tokens, num, &opcode);
            break;
        }

        case TYPE_MODCZ:
        {
            value = GetModczParm(&i, tokens, num);
            opcode |= (value & 15) << 13;
            if (CheckExpected(",", i, tokens, num)) break;
            i++;
            value = GetModczParm(&i, tokens, num);
            opcode |= (value & 15) << 9;
            i--;
            ProcessWx(&i, tokens, num, &opcode);
            break;
        }

        case TYPE_BITL:
        {
            if (EvaluateExpression(12, &i, tokens, num, &value, &is_float)) break;
	    opcode |= (value & 511) << 9;
	    i++;
            if (CheckExpected(",", i, tokens, num)) break;
            ProcessSrcWcz(&i, tokens, num, &opcode);
            break;
        }

        case TYPE_DIRL:
        {
	    value = EncodeAddressField(&i, tokens, num, 1, opcode, 1);
	    opcode |= (value & 0x1ff) << 9;
	    if (value & 0x200) opcode |= I_BIT;
            ProcessWcz(&i, tokens, num, &opcode);
            break;
        }

        case TYPE_TESTP:
        {
	    value = EncodeAddressField(&i, tokens, num, 1, opcode, 1);
	    opcode |= (value & 0x1ff) << 9;
	    if (value & 0x200) opcode |= I_BIT;
            ProcessWlx(&i, tokens, num, &opcode, 1);
            break;
        }

        case TYPE_TESTB:
        {
            if (EvaluateExpression(12, &i, tokens, num, &value, &is_float)) break;
	    opcode |= (value & 511) << 9;
	    i++;
            if (CheckExpected(",", i, tokens, num)) break;
            ProcessSrcWlx(&i, tokens, num, &opcode);
            break;
        }

        case TYPE_POLL:
        {
            i--;
            ProcessWx(&i, tokens, num, &opcode);
            break;
        }

        // Handle OP2XX instruction, such as not d[,s]
        case TYPE_OP2XX:
        {
            if (EvaluateExpression(12, &i, tokens, num, &value, &is_float)) break;
	    opcode |= (value & 511) << 9;
            if (i >= num - 1 || strcmp(tokens[i+1], ","))
            {
	        opcode |= (value & 511);
                ProcessWx(&i, tokens, num, &opcode);
            }
            else
            {
	        i++;
                ProcessSrc(&i, tokens, num, &opcode);
            }
            break;
        }

        // Handle OP2XY instruction, such as alts d[,s]
        case TYPE_OP2XY:
        {
            int is_alti = !strcmp(tokens[i-1], "alti");
            if (EvaluateExpression(12, &i, tokens, num, &value, &is_float)) break;
	    opcode |= (value & 511) << 9;
            if (i >= num - 1 || strcmp(tokens[i+1], ","))
            {
                if (is_alti) opcode |= 0x164;
                opcode |= I_BIT;
                ProcessWx(&i, tokens, num, &opcode);
            }
            else
            {
	        i++;
                ProcessSrc(&i, tokens, num, &opcode);
            }
            break;
        }

        // Handle REPXX instruction, such as rep d,s
        case TYPE_REPXX:
        {
            int rflag = !strcmp(tokens[i], "@");

            if (rflag || !strcmp(tokens[i], "#"))
            {
                i++;
                opcode |= Z_BIT;
            }
            if (EvaluateExpression(12, &i, tokens, num, &value, &is_float)) break;
            if (rflag)
            {
                if (hubmode)
                    value = ((value - hub_addr) >> 2) - 2;
                else
                    value -= (cog_addr >> 2) + 1;
            }
	    opcode |= (value & 511) << 9;
            if (CheckExpected(",", ++i, tokens, num)) break;
            ProcessSrc(&i, tokens, num, &opcode);
            break;
        }

        // Handle OP1D instruction, such as locknew d
        case TYPE_OP1D:
        {
            if (EvaluateExpression(12, &i, tokens, num, &value, &is_float)) break;
	    opcode |= (value & 511) << 9;
            ProcessWx(&i, tokens, num, &opcode);
            break;
        }

        // Handle OP1DOPT instruction, such as getrnd [d]
        case TYPE_OP1DOPT:
        {
            if (strcmp(tokens[i], "wz") && strcmp(tokens[i], "wc"))
            {
                if (EvaluateExpression(12, &i, tokens, num, &value, &is_float)) break;
	        opcode |= (value & 511) << 9;
            }
            else
                i--;
            ProcessWx(&i, tokens, num, &opcode);
            break;
        }

        // Handle OP0 instruction, such as ret
        case TYPE_OP0:
        {
            i--;
            ProcessWx(&i, tokens, num, &opcode);
            break;
        }

        // Handle OP2B instruction, such as rdfast d/#,s/#
        case TYPE_OP2B:
        {
	    value = EncodeAddressField(&i, tokens, num, 1, opcode, 1);
	    opcode |= (value & 0x1ff) << 9;
	    if (value & 0x200) opcode |= Z_BIT;
            if (CheckExpected(",", ++i, tokens, num)) break;
            ProcessSrc(&i, tokens, num, &opcode);
            break;
        }

        // Handle AKPIN instruction, such as akpin s/#
        case TYPE_AKPIN:
        {
            ProcessSrc(&i, tokens, num, &opcode);
            break;
        }

        // Handle OP3AX instruction, such as getnib D,S/#,#n
        case TYPE_OP3AX:
        {
	    value = EncodeAddressField(&i, tokens, num, 0, opcode, 1);
	    if (value < 0 || value > 511) break;
	    opcode |= (value & 0x1ff) << 9;
	    if (++i >= num) break;
            if (CheckExpected(",", i, tokens, num)) break;
	    if (++i >= num) break;
	    value = EncodeAddressField(&i, tokens, num, 2, opcode, 0);
	    if (value < 0) break;
	    if (value & 0x200) opcode |= I_BIT;
	    opcode |= (value & 0x1ff);
            if (CheckExpected(",", ++i, tokens, num)) break;
            if (CheckExpected("#", ++i, tokens, num)) break;
            i++;
	    if (EvaluateExpression(12, &i, tokens, num, &value, &is_float)) break;
	    opcode |= (value & 7) << 19;
            ProcessWx(&i, tokens, num, &opcode);
            break;
        }

        // Handle OP3BX instruction, such as setnib D,S/#,#n
        case TYPE_OP3BX:
        {
	    value = EncodeAddressField(&i, tokens, num, 0, opcode, 1);
	    if (value < 0 || value > 511) break;
	    opcode |= (value & 0x1ff) << 9;
            if (CheckExpected(",", ++i, tokens, num)) break;
	    if (++i >= num) break;
	    value = EncodeAddressField(&i, tokens, num, 2, opcode, 0);
	    if (value < 0) break;
	    if (value & 0x200) opcode |= I_BIT;
	    opcode |= (value & 0x1ff);
            if (CheckExpected(",", ++i, tokens, num)) break;
            if (CheckExpected("#", ++i, tokens, num)) break;
            i++;
	    if (EvaluateExpression(12, &i, tokens, num, &value, &is_float)) break;
	    opcode |= (value & 7) << 19;
            ProcessWx(&i, tokens, num, &opcode);
            break;
        }

        // Handle OP2PX instruction, such as rdlong D,S/#/PTRx
        case TYPE_OP2PX:
        {
	    value = EncodeAddressField(&i, tokens, num, 0, opcode, 1);
	    if (value < 0 || value > 511) break;
	    opcode |= (value & 0x1ff) << 9;
            if (CheckExpected(",", ++i, tokens, num)) break;
            ProcessPointerSrc(&i, tokens, num, &opcode);
            break;
        }

        // Handle OP2AX instruction, such as djnz D,S/#rel9
        case TYPE_OP2AX:
        {
            if (EvaluateExpression(12, &i, tokens, num, &value, &is_float)) break;
	    opcode |= (value & 511) << 9;
            if (CheckExpected(",", ++i, tokens, num)) break;
            ProcessRsrc(&i, tokens, num, &opcode);
            break;
        }

        // Handle register for OP2EX instruction, such as loc reg,#abs/#rel
        case TYPE_OP2EX:
        {
            int is_loc = !strcmp(tokens[i-1], "loc");
            if (EvaluateExpression(12, &i, tokens, num, &value, &is_float)) break;
            if (CheckExpected(",", ++i, tokens, num)) break;
            if (strcmp(tokens[i+1], "#") || value < 0x1f6 || value > 0x1f9)
            {
//printf("Trace 1\n");
                if (is_loc)
                {
                    PrintError("ERROR: Invalid LOC instruction\n", 0, 0);
                    break;
                }
                s = &SymbolTable[opindex+1];
                opcode = s->value | (opcode & 0xf0000000);
	        opcode |= (value & 511) << 9;
                ProcessRsrc(&i, tokens, num, &opcode);
            }
            else
            {
                int srcval;
                if (CheckExpected("#", ++i, tokens, num)) break;
//printf("Trace 2- tokens[%d] = %s\n", i, tokens[i+1]);
                if (!strcmp(tokens[i+1], "\\"))
                    srcval = 1;
                else
                    GetImmSrcValue(i, tokens, num, &srcval);
                if ((srcval&3) == 0 && srcval < (255 * 4) && srcval > (-256 * 4) && !is_loc)
                {
//printf("Trace 2\n");
                    i--;
                    s = &SymbolTable[opindex+1];
                    opcode = s->value | (opcode & 0xf0000000);
	            opcode |= (value & 511) << 9;
                    ProcessRsrc(&i, tokens, num, &opcode);
                }
                else
                {
//printf("Trace 3\n");
	            opcode |= ((value - 0x1f6) & 3) << 21;
                    ProcessBigSrc(&i, tokens, num, &opcode);
                }
            }
            break;
        }

        // Handle OP2CX instruction, such as wrbyte D/#,S/#/PTRx
        case TYPE_OP2CX:
        {
            CheckVref(i, tokens, num, 0);
            value = EncodeAddressField(&i, tokens, num, 1, opcode, 1);
            opcode |= (value & 0x1ff) << 9;
            if (value & 0x200) opcode |= Z_BIT;
            if (CheckExpected(",", ++i, tokens, num)) break;
            ProcessPointerSrc(&i, tokens, num, &opcode);
            break;
        }

        // Handle OP2DX instruction, such as jp D/#,S/#rel9
        case TYPE_OP2DX:
        {
            value = EncodeAddressField(&i, tokens, num, 1, opcode, 1);
            opcode |= (value & 0x1ff) << 9;
            if (value & 0x200) opcode |= Z_BIT;
            if (CheckExpected(",", ++i, tokens, num)) break;
            ProcessRsrc(&i, tokens, num, &opcode);
            break;
        }

        // Handle OPSREL9 instruction, such as jint S/#rel9
        case TYPE_OPSREL9:
        {
            i--;
            ProcessRsrc(&i, tokens, num, &opcode);
            break;
        }

        // Handle OP1AX instruction, such as jmp #abs/#rel
        case TYPE_OP1AX:
        {
            if (!strcmp(tokens[i], "#"))
                ProcessBigSrc(&i, tokens, num, &opcode);
            else
            {
                opcode = SymbolTable[opindex+1].value | (opcode & 0xf0000000);
                if (EvaluateExpression(12, &i, tokens, num, &value, &is_float)) break;
	        opcode |= (value & 511) << 9;
                ProcessWx(&i, tokens, num, &opcode);
            }
            break;
        }

        // Handle OP1B instruction, such as augs #23bits
        case TYPE_OP1B:
        {
            if (CheckExpected("#", i, tokens, num)) break;
            i++;
            EvaluateExpression(12, &i, tokens, num, &value, &is_float);
	    opcode |= (value >> 9) & 0x7fffff;
            ExpectDone(&i, tokens, num);
            break;
        }

        // Handle for OP1C instruction, such as cogstop d/#
        case TYPE_OP1C:
        {
	    value = EncodeAddressField(&i, tokens, num, 1, opcode, 1);
	    opcode |= (value & 0x1ff) << 9;
	    if (value & 0x200) opcode |= I_BIT;
            ProcessWx(&i, tokens, num, &opcode);
            break;
        }

        case TYPE_CON:
        {
            PrintError("ERROR: Invalid opcode \"%s\"\n", tokens[i]);
            break;
        }

        // Catch unsupported types
        default:
        {
            printf("ERROR: Opcode type %d is not supported\n", optype);
            break;
        }
    }

    if (pass == 2)
    {
        PrintIt(printflag, hub_addr, cog_addr, data_size, buffer2, &opcode);
        DumpIt(printflag, &opcode, 4);
    }
 
    // Update the address
    cog_addr += cog_incr;
    hub_addr += hub_incr;
}

void PrintIt(int printflag, int hub_addr, int cog_addr, int data_size, char *buffer2, void *ptr)
{
    unsigned char *value = ptr;

    if (printlevel == 0) return;

    if (printlevel >= 3)
    {
        if (hubmode)
            fprintf(lstfile, "%5.5x    ", hub_addr);
        else
            fprintf(lstfile, "%5.5x %3.3x", hub_addr, cog_addr >> 2);
    }

    if (printflag == PRINT_NOCODE)
    {
        fprintf(lstfile, "          %s\n", buffer2);
        return;
    }

    if (data_size == 0)
        fprintf(lstfile, "         ");
    else if (data_size == 1)
        fprintf(lstfile, " %2.2x      ", value[0]);
    else if (data_size == 2)
        fprintf(lstfile, " %2.2x%2.2x    ", value[1], value[0]);
    else if (data_size == 3)
        fprintf(lstfile, " %2.2x%2.2x%2.2x  ", value[2], value[1], value[0]);
    else
        fprintf(lstfile, " %2.2x%2.2x%2.2x%2.2x", value[3], value[2], value[1], value[0]);

    if (printlevel >= 2)
        fprintf(lstfile, " %s\n", buffer2);
    else
        fprintf(lstfile, "\n");

}

int CheckComment(char *buffer, int *pflag)
{
    int len = strlen(buffer);

    if (len == 0)
    {
        return *pflag;
    }
    else if (*pflag)
    {
        *pflag = (buffer[len-1] != '}');
        return 1;
    }
    else
    {
        char *ptr = SkipChars(buffer, " \t");
        if (*ptr == '{')
        {
            ptr = FindChar(ptr, '}');
            *pflag = (*ptr != '}');
        }
        return *pflag;
    }
}

int ProcessConstantLine(int currval, char **tokens, int num)
{
    int j;
    int i= 0;
    int is_float = -1;
    int commaflag = 0;

    if (num < 1) return currval;

    if (StrCompNoCase(tokens[0], "con"))
    {
        i++;
        currval = 0;
    }

    for (;i < num; i++)
    {
        if (commaflag)
        {
            commaflag = 0;
            if (CheckExpected(",", i, tokens, num)) break;
        }
        else
        {
            commaflag = 1;
            if (!strcmp(tokens[i], "#"))
            {
                if (i >= num - 1)
                {
                    fprintf(lstfile, "Expected a constant value\n");
                    break;
                }
                i++;
                EvaluateExpression(12, &i, tokens, num, &currval, &is_float);
            }
            else if (i < num - 1 && !strcmp(tokens[i+1], "="))
            {
                if (i >= num - 2)
                {
                    fprintf(lstfile, "Expected a constant value\n");
                    break;
                }
                j = i;
                i += 2;
                if (EvaluateExpression(12, &i, tokens, num, &currval, &is_float)) return currval;
                AddSymbol(tokens[j], currval++, (is_float ? TYPE_FLOAT : TYPE_CON));
            }
            else
            {
                AddSymbol(tokens[i], currval++, TYPE_CON);
            }
        }
    }

    return currval;
}

// Parse a Spin file and call ParseDat for lines in the DAT section
void Parse(int pass)
{
    int i, num;
    char *tokens[200];
    char buffer[600];
    int mode = MODE_CON;
    int commentflag = 0;
    int currval = 0;

    textmode = 1;
    cog_addr = 0;
    hub_addr = 0;

    while (ReadString(buffer2, 300, infile, unicode))
    {
        if (CheckComment(buffer2, &commentflag))
        {
	    if (pass == 2) fprintf(lstfile, "                   %s\n", buffer2);
            continue;
        }
	num = Tokenize(buffer2, tokens, 100, buffer);
	if (num > 0) i = SearchList(SectionKeywords, tokens[0]);
	else i = -1;
	if (i >= 0 || mode != MODE_DAT || num == 0)
	{
	    if (i >= 0) mode = i;
            if (i == MODE_DAT && num > 1)
                ParseDat(pass, buffer2, tokens, num);
	    else if (pass == 2)
                fprintf(lstfile, "                   %s\n", buffer2);
	    else if (mode == MODE_CON)
                currval = ProcessConstantLine(currval, tokens, num);
	    continue;
        }
        ParseDat(pass, buffer2, tokens, num);
    }
}

void usage(void)
{
    printf("usage: p2asm\n");
    printf("  [ -o ]     generate an object file\n");
    printf("  [ -d ]     enable debug prints\n");
    printf("  [ -c ]     enable case sensitive mode\n");
    printf("  [ -hub ]   write only hub code to object file\n");
    printf("  file       source file\n");
    exit(1);
}

int main(int argc, char **argv)
{
    int i;
    char *ptr;
    char *infname = 0;
    char rootname[80], lstfname[80], binfname[80];

    for (i = 1; i < argc; i++)
    {
	if (argv[i][0] == '-')
	{
	    if (strncmp(argv[i], "-p", 2) == 0)
	        printlevel = argv[i][2] - '0';
            else if(!strcmp(argv[i], "-o"))
                objflag = 1;
            else if(!strcmp(argv[i], "-d"))
                debugflag = 1;
            else if(!strcmp(argv[i], "-c"))
                case_sensative = 1;
            else if(!strcmp(argv[i], "-PIC"))
                picflag = 1;
            else if(!strcmp(argv[i], "-hub"))
                hubonly = 1;
	    else
	        usage();
	}
	else
           infname = argv[i];
    }

    if (!infname) usage();

    strcpy(rootname, infname);
    ptr = FindChar(rootname, '.');
    *ptr = 0;
    strcat(rootname, ".");
    strcpy(lstfname, rootname);
    strcpy(binfname, rootname);
    strcat(lstfname, "lst");
    strcat(binfname, "bin");

    infile = FileOpen(infname, "r");
    unicode = CheckForUnicode(infile);
    lstfile = FileOpen(lstfname, "w");
    binfile = FileOpen(binfname, "wb");
    if (objflag)
    {
        strcat(rootname, "o");
        objfile = FileOpen(rootname, "wb");
        fwrite("P2OBJECT", 1, 8, objfile);
        i = strlen(rootname)+1;
        fwrite(&i, 1, 1, objfile);
        fwrite(rootname, 1, i, objfile);
        if (hubonly)
            i = 0x400;
        else
            i = 0;
        fwrite(&i, 1, 4, objfile);
    }

    ReadSymbolTable();
    Parse(1);
    if (FindSymbol("main") >= 0) hasmain = 1;
    Parse(2);

    i = 0;
    if (!objflag)
    {
        while (bincount & 31) DumpIt(PRINT_CODE, &i, 1);
    }
    fclose(infile);
    fclose(lstfile);
    fclose(binfile);

    if (objflag)
    {
        int num;
        SymbolT *s;
        if (picflag)
        {
            for (i = 0; i < pictablenum; i++)
            {
                num = (pictableaddr + i) << 20;
                s = GetSymbolPointer(PicTableName[i]);
                if (s) num += s->value2;
                if (debugflag) printf("VDEF %8.8x %s\n", num, PicTableName[i]);
                WriteObjectEntry('V', num, PicTableName[i]);
            }
        }
        else
        {
            SymbolT *s;

            for (i = 0; i < numsym; i++)
            {
                s = &SymbolTable[i];
                if (s->type == TYPE_HUB_ADDR)
                {
                    if (debugflag) printf("VDEF %8.8x %s\n", s->value2, s->name);
                    WriteObjectEntry('V', s->value2, s->name);
                }
            }
        }
        i = 'E';
        fwrite(&i, 1, 1, objfile);
        fwrite(&hub_addr, 1, 4, objfile);
        binfile = FileOpen(binfname, "rb");
        if (hubonly)
            fread(databuffer, 1, 0x400, binfile);
        while (1)
        {
            num = fread(databuffer, 1, 1000, binfile);
            if (num <= 0) break;
            fwrite(databuffer, 1, num, objfile);
        }
        fclose(objfile);
    }

    if (debugflag) PrintSymbolTable(0);

    return 0;
}
