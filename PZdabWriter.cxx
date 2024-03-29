//
// File:        PZdabWriter.cxx
//
// Description: Routines to write zdab files
//              
// Revisions:   06/01/99 - PH Created - (Based on C code courtesy of Reda Tafirout)
//              01/05/00 - PH Added WriteBank() to write MAST'd banks
//              03/03/03 - PH Changed PmtEventRecord definition so it is now
//                            swapped like a set of 32-bit integers.  Set size
//                            of ZDAB bank in WriteBank() instead of Write().
//              03/14/03 - PH Added Close(), mError and MD5 checksum feature.
//              03/19/03 - PH Changed Flush() to flush records even if ZEBRA block
//                            isn't full.
//

#include <string.h>
#include <stdlib.h>
#include "PZdabWriter.h"
#include "CUtils.h"
#include "Record_Info.h"

//#define DEBUG_ZDAB

#define ADD_RECORD(x)       AddRecord((u_int32 *)(&x),sizeof(x)/sizeof(u_int32))

#define BASE_LINK           301     // value for base zebra link
#define SUPP_BANK_LINK      327     // address of supporting bank (up-link)

//===================================================================================
// Zebra bank information
// (eventually, all this could go into a data file to be read in at run time)
//

#define SNOMAN_VERSION      3.0190      // snoman version number for MAST bank
#define ORIGINAL_VERSION    3.0190      // original version number for MAST bank

// Note: The order of these entries corresponds to the EBankIndex enum in the header file
SBankDef PZdabWriter::sBankDef[NUM_BANKS] = {
    { ZDAB_RECORD,  6,                    0,  0, 0x00000, { 0x0002000c } }, // must fill in bank size
    { MAST_RECORD,  1, WORD_SIZE(SBankMAST), 25, 0x00004, { 0x00030025 } }, // must fill in links after iochar[0]
    { RHDR_RECORD,  5, WORD_SIZE(SBankRHDR),  0, 0x00000, { 0x0002000c } },
    { EPED_RECORD,  6, WORD_SIZE(SBankEPED),  0, 0x00000, { 0x0002000c } },
    { TRIG_RECORD,  7, WORD_SIZE(SBankTRIG),  0, 0x00000, { 0x0002000c } },
    { SOSL_RECORD,  8, WORD_SIZE(SBankSOSL),  0, 0x00000, { 0x0323000c } },
    { CAST_RECORD, 10, WORD_SIZE(SBankCAST),  0, 0x80000, { 0xc9a3000e, 0x40041803, 0x000e005a } },
    { CAAC_RECORD, 11, WORD_SIZE(SBankCAAC),  0, 0x00000, { 0x0003000c } },
};

//===================================================================================

/**********************************************************************************
** Here is some example code to read a zdab file with PZdabFile
** and write it out with PZdabWriter:
**
static void rewrite_zdab(char *infile, char *outfile)
{
    printf("opening file\n");
    FILE *fp = fopen(infile,"rb");
    if (!fp) {
        printf("error opening file\n");
        return;
    }
    PZdabFile r;
    PZdabWriter w(outfile);
    r.Init(fp);
    for (;;) {
        nZDAB *nzdab = r.NextRecord();
        if (!nzdab) break;
        int index = PZdabWriter::GetIndex(nzdab->bank_name);
        if (index < 0) {
            printf("Unrecognized bank %s\n",PZdabFile::BankNameString(nzdab->bank_name));
        // write the bank (will write after a MAST bank if necessary)
        } else if (w.WriteBank(r.GetBank(nzdab), index)) {
            printf("Write error -- aborting\n");
            break;
        }
    }
    fclose(fp);
    printf("done\n");
}
**********************************************************************************/

//*** open zdab file and reset counters ***//
PZdabWriter::PZdabWriter(char *file_name, int calcMD5)
{
    mBytesWritten = 0;
    mWritePos = 0;
    mError = 0;
    mCalcMD5 = calcMD5;
    if (mCalcMD5) {
        mMD5.Init();
    }
    
    // zero the buffer
    memset(mbuf, 0, sizeof(mbuf));
    
#ifdef DEBUG_ZDAB
    int block_count = 0;
#endif
    // ignore NULL and empty file names
    if (!file_name || *file_name=='\0') {
        zdaboutput = NULL;
        mError = 1;
        return;
    }
    strncpy((char *)zdab_output_file,file_name,MAX_NAMELEN);
    zdab_output_file[MAX_NAMELEN-1] = '\0';
    
    //FZ physical records counter
    irec = (u_int32)(-1);

    //check if file exists already and contains valid FZ structure
    zdaboutput = fopen(zdab_output_file,"r+b");
    if (zdaboutput) {
        printf("Output zdab file already exists. Scanning for last record...\n");
        for (;;) {
            if (fread(&mbuf,sizeof(mbuf),1,zdaboutput) != 1) {
                printf("Error: Existing zdab file has no End of Run signature\x07\n");
                printf("Can't write events to file %s\n",zdab_output_file);
                fclose(zdaboutput);
                zdaboutput = NULL;
                mError = 1;
                return;
            }
            SWAP_INT32(mbuf, NWREC);
            
            //check for physical records FZ signature
            if (mbuf[0] != ZEBRA_SIG0 ||
                mbuf[1] != ZEBRA_SIG1 ||
                mbuf[2] != ZEBRA_SIG2 ||
                mbuf[3] != ZEBRA_SIG3 )
            {
#ifdef DEBUG_ZDAB
                printf("Block %d: ERROR: Expected FZ signature!!!!!!\n",block_count++);
                continue;
            } else {
                printf("Block %d: Good FZ\n",block_count++);
#else
                printf("Error: Existing zdab file has wrong FZ signature\x07\n");
                printf("Can't write events to file %s\n",zdab_output_file);
                fclose(zdaboutput);
                zdaboutput = NULL;
                mError = 1;
                return;
#endif
            }
            // check end of run signal and reposition file pointer 
            if (mbuf[4] == 0x40000f00UL) {
                fseek(zdaboutput,-(long)sizeof(mbuf),SEEK_CUR);
                printf("Appending events to zdab file %s\n",zdab_output_file);
                break;
            }
            irec = mbuf[5]; // update record counter
            
            // read through fast blocks to next FZ structure
            u_int32 nfast = mbuf[7];
#ifdef DEBUG_ZDAB
            if (nfast) {
                printf("%d fast blocks\n",nfast);
            }
#endif
            while (nfast) {
                if (fread(&mbuf,sizeof(mbuf),1,zdaboutput) != 1) {
                    printf("Error: Existing zdab file is corrupt\x07\n");
                    printf("Can't write events to file %s\n",zdab_output_file);
                    fclose(zdaboutput);
                    zdaboutput = NULL;
                    mError = 1;
                    return;
                }
#ifdef DEBUG_ZDAB
                if (mbuf[0] == ZEBRA_SIG0 &&
                    mbuf[1] == ZEBRA_SIG1 &&
                    mbuf[2] == ZEBRA_SIG2 &&
                    mbuf[3] == ZEBRA_SIG3 )
                {
                    printf("Block %d: ERROR: Fast block has FZ signature!!!!!!\n",block_count++);
                } else {
                    printf("Block %d: Good fast block\n", block_count++);
                }
#endif
                --nfast;
            }
        }
    } else {
        zdaboutput = fopen(zdab_output_file,"wb");
        if (zdaboutput) {
            printf("Created output zdab file %s\n",zdab_output_file);
// test to see if a bigger buffer improves throughput
//          setvbuf(zdaboutput,NULL,_IOFBF,65536L);
        } else {
            printf("Error creating output zdab file %s\x07\n",zdab_output_file);
        }
    }
/*
** initialize static fields of our record structures
*/
    // physical record signature
    mpr[0] = ZEBRA_SIG0;
    mpr[1] = ZEBRA_SIG1; 
    mpr[2] = ZEBRA_SIG2;
    mpr[3] = ZEBRA_SIG3;
    mpr[4] = NWREC;
    mpr[5] = ++irec;
    mpr[6] = NPHREC;
    mpr[7] = 0;

    // logical record type
    mlr[1] = 2;

    // pilot record information (signature and ZEBRA version)
    mpili[0] = 0x4640e400UL;
    mpili[1] = 37700;
    mpili[2] = 0;
    mpili[3] = 0;
    mpili[4] = 0;
    mpili[5] = 0;
    mpili[6] = 0;   // this is 2 for MAST banks
    mpili[7] = 0;
    mpili[8] = 0;   // this is 'SUPP_BANK_LINK' for MAST banks
    mpili[9] = 0;
    mpili[10]= BASE_LINK;   // 1st link of MAST relocation table (constant)

    // bank information (name, etc.) 
    // (all but mbk[0] must be set for the specific bank type)
    mbk[0] = 0; // constant
    mbk[1] = 0;
    mbk[2] = 0;
    mbk[3] = 0;
    mbk[4] = 0;
    mbk[5] = 0;
    mbk[6] = 0;
    mbk[7] = 0;
    mbk[8] = 0;

    // end of run record
    meor[0] = 1;   // record length
    meor[1] = 1;   // record type
    meor[2] = 0;   // end of run
    
    // end of zebra record
    meoz[0] =  4; // record length
    meoz[1] =  1; // record type
    meoz[2] = (u_int32)(-1); // end of zebra
    meoz[3] =  0;
    meoz[4] =  0;
    meoz[5] = 73;
    
    // mast bank data
    mMastData.currentVersion = SNOMAN_VERSION;
    mMastData.originalVersion = ORIGINAL_VERSION;
    
    // add first steering block to the buffer
    ipos = 0;
    ADD_RECORD(mpr);
}

//*** ZEBRA end of run/file signature (has to be on a steering block) ***//
PZdabWriter::~PZdabWriter()
{
    Close();
}

/* close the file - returns zero on success or non-zero if any error occurred while writing */
int PZdabWriter::Close()
{
    if (zdaboutput) {
        //complete the current physical record with a padding record
        WritePhysicalRecord();

        // start a new steering block and signal end_of_run
        mpr[4] = 0x40000f00UL;
        mpr[5] = 0;
        mpr[6] = NPHREC;
        mpr[7] = 0;
        ADD_RECORD(mpr);
        
        //end of RUN signature
        ADD_RECORD(meor);

        //complete with a padding and write record
        WritePhysicalRecord();

        //end of "ZEBRA" file signature (on another steering block)
        ADD_RECORD(mpr);
        ADD_RECORD(meoz);

        //complete with a padding record and write physical record
        WritePhysicalRecord();

        //end of DATA (system EOF)
        if (fclose(zdaboutput)) {
            printf("Error closing output zdab file %s\n",zdab_output_file);
            mError = 1;
        } else {
            printf("Closed output zdab file %s\n",zdab_output_file);
        }
        zdaboutput = NULL;
        
    }
    return(mError);
}

// get array index for specified bank
// - returns -1 if bank is not recognized
int PZdabWriter::GetIndex(u_int32 bank_name)
{
    for (int i=0; i<NUM_BANKS; ++i) {
        if (bank_name == sBankDef[i].name) return(i);
    }
    return(-1);
}

// return size of bank given the writer bank index
// Note: Index is not range checked.
int PZdabWriter::GetBankNWords(int index)
{
    return(sBankDef[index].nwords);
}

// WriteBank - write an arbitrary bank to the file
// - returns 0 on success
int PZdabWriter::WriteBank(u_int32 *bank_ptr, int index)
{
    int i, nsize, hdr_size, nfast, nio_nl;
    int npilot, mast_nio_nl = 0, fast = 0;
    
    if (!zdaboutput) {
        printf("Zdab output file not open!\n");
        return(-1);
    }
    
    // don't write MAST banks alone
    // they will be written automatically before the appropriate banks
    if (index == kMASTindex) return(0);
    
    // must set the size of PMT event records (since it is variable)
    if (index == kZDABindex) {
        sBankDef[kZDABindex].nwords = PZdabFile::GetSize((PmtEventRecord *)bank_ptr) / sizeof(u_int32);
        // KL 12/05/13 - Swap nhit to get chopper to work
        SWAP_INT32(bank_ptr+3,1);
    }
    
    // get the size of the record to be written
    nsize = sBankDef[index].nwords;
    // get the number of i/o control words and links
    nio_nl = (int)(sBankDef[index].iochar[0] & 0x0000ffff) - 12;

    // calculate size of bank header including i/o control and link words
    hdr_size = 1 + nio_nl + NBANK;
    
    if (index != kZDABindex) {
        // add size of MAST bank (goes before all but ZDAB banks)
        mast_nio_nl = (int)(sBankDef[kMASTindex].iochar[0] & 0x0000ffff) - 12;
        hdr_size += 1 + mast_nio_nl + NBANK + sBankDef[kMASTindex].nwords;
        mpili[6] = 2;                               // 2 entries in relocation table
        mpili[8] = SUPP_BANK_LINK;                  // entry link
        mpili[11] = BASE_LINK + hdr_size + nsize;   // 2nd relocation table entry
    } else {
        mpili[6] = 0;           // no relocation table
        mpili[8] = 0;           // no entry link
    }
    npilot = NPILOT + mpili[6]; // write relocation table with pilot record

    // start a logical record if we have already written the steering block and the
    // next record will need a fast block
    if (mWritePos && ipos + NLOGIC + npilot + hdr_size + nsize > 2 * NWREC - NPHREC) {
        if (WritePhysicalRecord()) {
            fclose(zdaboutput);
            zdaboutput = NULL;
            mError = 1;
            printf("Error writing to output zdab file %s!  File closed.\x07\n",zdab_output_file);
            return(-2);
        }
        mpr[5] = ++irec;
        mpr[6] = NPHREC;
        mpr[7] = 0;
        ADD_RECORD(mpr);
    }

    //do not start a logical record if not enough room
    //(otherwise complete the steering block with a padding block)
    if ( ipos >= (u_int32)(NWREC-NLOGIC-npilot-hdr_size) ) {
        if (WritePhysicalRecord()) {
            fclose(zdaboutput);
            zdaboutput = NULL;
            mError = 1;
            printf("Error writing to output zdab file %s!  File closed.\x07\n",zdab_output_file);
            return(-2);
        }
        mpr[5] = ++irec;
        mpr[6] = NPHREC;
        mpr[7] = 0;
        ADD_RECORD(mpr);
    }

    // logical record info (size and data type)
    mlr[0] = npilot + hdr_size + nsize;
    ADD_RECORD(mlr);

    // Pilot record info (mostly constant except for bank material size)
    mpili[7] = hdr_size + nsize;
    AddRecord(mpili, npilot);

    // add MAST bank if necessary
    if (index != kZDABindex) {
        // first clear all the mast links
        int nlinks = sBankDef[kMASTindex].nlinks;
        memset(sBankDef[kMASTindex].iochar + 1 + (mast_nio_nl - nlinks), 0, nlinks * sizeof(u_int32));
        // then set the link for the bank we are writing
        sBankDef[kMASTindex].iochar[1 + mast_nio_nl - sBankDef[index].id] 
            = BASE_LINK + 1 + mast_nio_nl + NBANK + sBankDef[kMASTindex].nwords + 1 + nio_nl;
        // add the i/o characteristic for the MAST bank
        AddRecord(sBankDef[kMASTindex].iochar, mast_nio_nl + 1);
        
        // add MAST Bank info
        mbk[1] = 0;
        mbk[2] = 0;
        mbk[3] = sBankDef[kMASTindex].id;
        mbk[4] = sBankDef[kMASTindex].name;
        mbk[5] = sBankDef[kMASTindex].nlinks;
        mbk[6] = sBankDef[kMASTindex].nlinks;
        mbk[7] = sBankDef[kMASTindex].nwords;
        mbk[8] = sBankDef[kMASTindex].status;
        ADD_RECORD(mbk);
        
        // add the MAST bank data
        ADD_RECORD(mMastData);
    }
    
    // add the i/o characteristic for the bank we are writing
    AddRecord(sBankDef[index].iochar, nio_nl + 1);
    
    // add Bank info
    mbk[1] = SUPP_BANK_LINK;
    mbk[2] = SUPP_BANK_LINK - sBankDef[index].id;
    mbk[3] = sBankDef[index].id;
    mbk[4] = sBankDef[index].name;
    mbk[5] = sBankDef[index].nlinks;
    mbk[6] = sBankDef[index].nlinks;
    mbk[7] = nsize;
    mbk[8] = sBankDef[index].status;
    ADD_RECORD(mbk);
    
    // byte swap the bank to the external format
    if(index!=kZDABindex){
        SWAP_INT32(bank_ptr, nsize);
    }    

    // write the bank data
    for (i=0; i<nsize; ++i) { 
        if (ipos > NWREC-1) {
            // start new physical record (depends on how many data is left)
            // check if it is a steering or a fast block
            int nleft = nsize - i;
            if (nleft > (NWREC-NPHREC)) {
                // compute number of fast blocks required
                if (!fast) {
                    // be sure our steering record hasn't been written
                    if (mWritePos >= 7) {
                        fclose(zdaboutput);
                        zdaboutput = NULL;
                        mError = 1;
                        break;
                    }
                    fast = 1;
//                  nfast = (nleft-(NWREC-NPHREC-NLOGIC))/NWREC + 1;
// careful!  fixed fast block calculation problem - PH 06/09/99
                    nfast = (nleft-(NWREC-NPHREC)-1)/NWREC + 1; 
                    mbuf[7] = nfast;
                    SWAP_INT32(mbuf+7, 1);
                }
                if (FWrite(&mbuf,sizeof(mbuf))) {
                    fclose(zdaboutput);
                    zdaboutput = NULL;
                    mError = 1;
                    break;
                }
                ipos = 0;
            } else {
                fast = 0;
                if (FWrite(&mbuf,sizeof(mbuf))) {
                    fclose(zdaboutput);
                    zdaboutput = NULL;
                    mError = 1;
                    break;
                }
                ipos = 0;
                mpr[5] = ++irec;
                mpr[6] = NPHREC + (nsize - i);
                ADD_RECORD(mpr);
            } 
        }
        mbuf[ipos] = bank_ptr[i];
#ifdef DEBUG_ZDAB
        if (mpr[0] != ZEBRA_SIG0) {
            printf("ZDAB Buffer overrun!!!\n");
        }
#endif
        // (already swapped)
        ++ipos;
    }
    
    // byte swap the bank back again
    SWAP_INT32(bank_ptr, nsize);
    
    if (mError) {
        printf("Error writing to output zdab file %s!  File closed.\x07\n",zdab_output_file);
        return(-1);
    }

    // check if the remaining data ended on a fast block 
    if (fast) {
        // fill in with a padding block
        if (WritePhysicalRecord()) {
            fclose(zdaboutput);
            zdaboutput = NULL;
            mError = 1;
            printf("Error writing to output zdab file %s!  File closed.\x07\n",zdab_output_file);
            return(-1);
        }
        mpr[5] = ++irec;
        mpr[6] = NPHREC;
        mpr[7] = 0;
        ADD_RECORD(mpr);
        fast = 0;
    }

    return(0);
}


/* add a record to our buffer */
void PZdabWriter::AddRecord(u_int32 *data, u_int32 nwords)
{
    memcpy(mbuf+ipos, data, nwords * sizeof(u_int32));
    SWAP_INT32(mbuf+ipos, nwords);
    
    ipos += nwords; // update buffer pointer
}

/* write data to file */
int PZdabWriter::FWrite(void *buff, unsigned long size)
{
    if (mWritePos && size == sizeof(mbuf)) {
        if (mWritePos >= NWREC) {
            // we already wrote it all
            mWritePos = 0;
            return(mError);
        }
        buff = (void *)(mbuf + mWritePos);
        size -= (mWritePos * sizeof(u_int32));
        mWritePos = 0;      // reset write position since we wrote it all
    }
    if (fwrite(buff,size,1,zdaboutput) != 1) {
        mError = 1;
    } else {
        mBytesWritten += size;
        if (mCalcMD5) mMD5.Update((BYTE *)buff, size);
    }
    return(mError);
}


// flush the output file
int PZdabWriter::Flush()
{
    int     err = 0;
    
    // write any banks currently in buffer
    if (ipos > mWritePos) {
        err = FWrite(mbuf + mWritePos, (ipos - mWritePos) * sizeof(u_int32));
        mWritePos = ipos;
    }
    // flush the file
    if (!err) {
        err = fflush(zdaboutput);
    }
    if (err) {
        fclose(zdaboutput);
        zdaboutput = NULL;
        mError = 1;
        printf("Error flushing output zdab file %s!  File closed.\x07\n",zdab_output_file);
    }
    return(err);
}

/* add a padding record to buffer and write to file */
/* returns 0 on success */
int PZdabWriter::WritePhysicalRecord()
{
    if (ipos < NWREC - 1) {
        mbuf[ipos] = NWREC - ipos - 1; // Length of this padding record
        mbuf[ipos+1] = 5; // RecordID of a padding record
        memset(mbuf+ipos+2, 0, sizeof(u_int32)*(NWREC - ipos - 2));
        SWAP_INT32(mbuf+ipos, 2);
    } else if (ipos < NWREC) {
        mbuf[ipos] = 0; // write a 1-word padding record
    }
    ipos = 0;   // reset buffer pointer
    return(FWrite(&mbuf, sizeof(mbuf)));
}


