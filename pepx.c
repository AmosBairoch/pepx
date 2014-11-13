#include <stdio.h>
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
//#include <sys/syscall.h>
#include <errno.h>
#include <getopt.h>

#define FALSE 0
#define TRUE  1
#define MAXSEQ 20500
#define MAXSEQSIZE 36500
#define MAXISO 100
#define MAXVARINPEP 63
#define LINELEN 1024
#define ACLEN 16
#define BINSIZE 32  // adjust w stats
//#define BINSIZE 8  // adjust w stats
// optimal binsizes: 2->1000, 3->100, 4->25, 5->10, 6->?
#define MINPEPSIZE 3
#define MAXPEPSIZE 6 // Allowing 7 implies adapting BINSIZE t0 low value in 7-mers (mempry pbs)
#define SILENT 1
#ifndef max
	#define max( a, b ) ( ((a) > (b)) ? (a) : (b) )
#endif

// Globals

typedef struct idxdata
       {
       FILE*  fh;
       FILE*  ffh;
       int elemcnt;
       } IDXDATA;

IDXDATA idxinfo[MAXPEPSIZE], idxinfoIL[MAXPEPSIZE];
FILE* currentindex;
char currentresult[32];
static int currentpepsize; // weird but alters idxinfo[6] when changed if not static

typedef struct bin
       {
       int  ac_cnt;
       struct bin* nextbinptr;
       char ac[BINSIZE][ACLEN];
       } BIN;

BIN** toplevel[MAXPEPSIZE+1];

int varindex_overflow_cnt = 0;
char variants[MAXSEQSIZE][4];
char results[MAXSEQ][ACLEN];
char resultsIL[MAXSEQ][ACLEN];
int aacode[26] = {0,2,1,2,3,4,5,6,7,-1,8,9,10,11,-1,12,13,14,15,16,1,17,18,-1,19,3};
// B is D, Z is E, U is C
char aarevcode[] = "ACDEFGHIKLMNPQRSTVWY";
char matchmode[10] = "ACISO";
int pow20[] = {1,20,400,8000,160000,3200000,64000000,1280000000};
int binsizes[] = {0,0,1000,100,25,10,6,4};
char masterseq[MAXSEQSIZE];
char currAC[ACLEN];
char currISO[ACLEN];
char outputmode[16];
char linesep[4] = "\n";
char envstring[LINELEN];
char version[] = "1.11";
// debug/profiling/stats stuff
int debug;
int totalbins=0;
clock_t start;
double duration;
int umatchcnt;
int maxoccur=0;
int usscnt= 0;
int totalvarcnt = 0;
int varmisscnt = 0;
int ignore_variants = 0;
int IL_merge = 0;
char varfolder[LINELEN];
char indexfolder[LINELEN]=".";
char RESTserver[LINELEN]="http://www.nextprot.org";
FILE* varfh = NULL;

// ---------------- code2sseq ---------------------
void code2sseq(int sseqcode, int pepsize, char* subseq)
{
char sseq[16]="";
int i, j;

for(i=pepsize-1;i>=0;i--)
   {
   j = sseqcode / pow20[i];
   sseqcode %= pow20[i];
   strncat(sseq,&aarevcode[j],1);
   }
strcpy(subseq,sseq);
}

// ---------------- sscode ---------------------
int sscode(char* subseq)
{
int sseqcode = 0, len, i;
len = strlen(subseq);
for(i=len;i>0;i--)
  // subsq[--i]?
  sseqcode += pow20[len-i] * (aacode[subseq[i-1] - 'A']);
return(sseqcode);
}

// ---------------- pepx_build_varindex ---------------------
int pepx_build_varindex(char* iso)
{
char buf[LINELEN], orgAA[16], lastorgAA[16], varAA[16], AAbuf[LINELEN], fname[64], command[256], *ptr;
int i, skipvar = 0, cmdres, fpos, lastfpos, lpos, localvarcnt=0;
FILE *NextProtvariants;

if(debug)
  fprintf(stderr,"Building variants for %s\n",currISO);
// Get variants from NextProt
//sprintf(command,"wget -q \"http://www.nextprot.org/rest/isoform/NX_%s/variant?format=json\" -O NX_variants.txt",iso);
sprintf(fname,"%s/%s.txt",varfolder,iso);
//sprintf(fname,"/share/sib/common/Calipho/alain/variants/%s.txt",iso);
if((NextProtvariants=fopen(fname,"r"))==NULL)
  {
  sprintf(command,"wget -q \"%s/rest/isoform/NX_%s/variant?format=json\" -O %s",RESTserver,iso,fname);
  cmdres = system(command);
  if(cmdres != 0)
    {
    fprintf(stderr, "system call to %s failed, errno = %d\n", command,errno);
    exit(6);
    }
  else if((NextProtvariants=fopen(fname,"r"))==NULL)
    {
    perror(fname);
    fprintf(stderr,"%s\n",command);
    exit(7);
    //return(0);
    }
  }
  
// Reset table each time  
memset(variants,0,MAXSEQSIZE*4);
 
while(fgets(buf,LINELEN,NextProtvariants))
     {
     //printf("%s",buf);
     if(ptr=strstr(buf,"original_sequence"))
       {
       ptr += 21;
       strcpy(AAbuf,ptr);
       *strchr(AAbuf,'"')=0;
       if(strlen(AAbuf) < 3)
	 strcpy(orgAA,AAbuf);
       }
     else if(ptr=strstr(buf,"variant_sequence"))
       {
       ptr += 20;
       strcpy(AAbuf,ptr);
       *strchr(AAbuf,'"')=0;
       if(strlen(AAbuf) < 3)
	 strcpy(varAA,AAbuf);
       }
     else if(ptr=strstr(buf,"first_position"))
       fpos = atoi(ptr + 16);
     else if(ptr=strstr(buf,"last_position"))
       lpos = atoi(ptr + 15);
     else if(strstr(iso,"P04637") && strstr(buf,"description") && strstr(buf,"sporadic"))
        // P53 has more than 1 variant per AA -> filter
       skipvar = 1;
     else if(strstr(buf,"identifiers"))
       {
       fgets(buf,LINELEN,NextProtvariants);
       fgets(buf,LINELEN,NextProtvariants);
       if(strchr(buf,']'))
	 // This is a MUTAGEN -> not valid snp
         continue;
       if(skipvar || fpos==1)
	 // No variants on Met-1 allowed
         {
	 skipvar = 0;
         continue;
	 }
       // Check is true SNP
       if((fpos != lpos) || strchr(varAA,'*'))
         {
	 if((strlen(orgAA) == 2) && (strlen(varAA) == 2) && orgAA[0] == varAA[0])// like A7XYQ1(180-181)
	   // Store variant at 2nd pos
	   {
	   if(!strchr(varAA,'*'))  // like A8MZ36(163-164) or worst P21359(2308-2309)
	     {//fprintf(stderr,"%s \n",varAA);
	     variants[fpos][0]=varAA[1];
	     if(varfh)
	       fprintf(varfh,"%s\t%s-%d-%c\n",iso,orgAA,fpos,varAA[1]);
	     localvarcnt++;
	     }
	   }
	 else if((strlen(orgAA) == 2) && (strlen(varAA) == 1) && varAA[0] == orgAA[1]) // like A6NFR6-3
	     // Store variant
	   {
	   if((fpos == lastfpos+1) && (orgAA[0] == lastorgAA[0])) // To avoid damage like in O00555-1 2219-2220 and 2220-2221
	     {
	     lastfpos = fpos;
	     continue;
	     }
	   variants[fpos-1][0]='X';
	   if(varfh)
	     fprintf(varfh,"%s\t%c-%d-X\n",iso,orgAA[0],fpos);
	   strcpy(lastorgAA,orgAA);
	   lastfpos = fpos;
	   localvarcnt++;
	   }
	 
	 //else  
           //fprintf(stderr,"%s variant %d-%d\n",currISO,fpos,lpos);
	 }
       else
	 // Store variant
	 {
	 if(strlen(varAA) == 0)
	   // code for 'Missing' AA
	   strcpy(varAA,"X");
	 for(i=0;i<4;i++)
	    // Check if we already have a variant at this pos
	    if(variants[fpos-1][i] == 0)
	      break;
	 //if(i) fprintf(stderr,"%s: multiple variants at %d-%d\n",currISO,fpos,lpos); 
	 if(varfh)
           fprintf(varfh,"%s\t%s-%d-%c\n",iso,orgAA,fpos,varAA[0]);
	 variants[fpos-1][i]=varAA[0];
	 localvarcnt++;
	 }
       }
     }

fclose(NextProtvariants);
// delete file to be sure that it is not reloaded in case next query fails
//sprintf(command,"rm NX_variants.txt");
//system(command);
//if(localvarcnt == 0)
  //fprintf(stdout,"%d variant in %s\n",localvarcnt,currISO);
if(debug)
  fprintf(stderr,"Variants done.\n");
  
return(localvarcnt);
}

// ---------------- pepx_loadall ---------------------
void pepx_loadall()
{
int i, pepcnt, filelen;
char fname[LINELEN], *ptr, idxpath[128]="";
FILE* idx;

if(strlen(indexfolder) > 8)
  {
  strcpy(idxpath,indexfolder);
  strcat(idxpath,"/");
  }
else if((ptr=getenv("PEPX")) != NULL)
  {
  strcpy(idxpath,ptr);
  strcat(idxpath,"/");
  }
for(i=MINPEPSIZE; i<= MAXPEPSIZE;i++)
   {
   usscnt = 0;
   sprintf(fname,"%spepx%d.idx",idxpath,i);
   if((idx=fopen(fname,"r"))==NULL)
     {
     perror(fname);
     exit(2);
     }
   // save flat file handle
   idxinfo[i].ffh = idx;
   strcat(fname,"2");
   if((idx=fopen(fname,"r"))==NULL)
     {
     perror(fname);
     exit(2);
     }
   fseek(idx,0,SEEK_END);
   filelen = ftell(idx);
   pepcnt = filelen/(i+11);     
   //printf("%d-mers size: %d bytes -> %d elts\n",i,filelen,pepcnt);
   idxinfo[i].fh = idx;
   idxinfo[i].elemcnt = pepcnt;
   if(IL_merge)
     {
     sprintf(fname,"%spepxIL%d.idx",idxpath,i);
     if((idx=fopen(fname,"r"))==NULL)
       {
       perror(fname);
       exit(2);
       }
     // save flat file handle
     idxinfoIL[i].ffh = idx;
     strcat(fname,"2");
     if((idx=fopen(fname,"r"))==NULL)
       {
       perror(fname);
       exit(2);
       }
     fseek(idx,0,SEEK_END);
     filelen = ftell(idx);
     pepcnt = filelen/(i+11);     
     //printf("IL: %d-mers size: %d bytes -> %d elts\n",i,filelen,pepcnt);
     idxinfoIL[i].fh = idx;
     idxinfoIL[i].elemcnt = pepcnt;
     }
   }
}

// ---------------- pepx_initindexes ---------------------
void pepx_initindexes()
{
int pepsize;
BIN **ptr;

for(pepsize=MINPEPSIZE; pepsize <= MAXPEPSIZE;pepsize++)
   {
   if((ptr = calloc(1, pow20[pepsize] * sizeof(BIN*))) == NULL)
     {
     fprintf(stderr,"Allocation failed\n");
     exit(11);
     }

   //printf("%d empty ints reserved at address %p\n",pow20[pepsize],ptr);
   toplevel[pepsize] = ptr;
   }
}

// ---------------- pepx_save ---------------------
void pepx_save(char* fname, int pepsize)
{
int sseqcode, i, maxcode, ac_cnt, iso_cnt;
char ac[ACLEN], ac_noiso[ACLEN], lastac[ACLEN], subseq[16], maxseq[16]="";
FILE *out, *out2;
BIN *binptr, **binarray;
int idxpos;

if((out=fopen(fname,"w"))==NULL)
 {
 perror(fname);
 exit(2);
 }

// secundary index
strcat(fname,"2");
if((out2=fopen(fname,"w"))==NULL)
 {
 perror(fname);
 exit(2);
 }
// index header line (1rst pep line must not be at pos 0 for new index schema)
for(i=0;i<pepsize+10;i++)
   fprintf(out2,"0");
fprintf(out2,"\n");

usscnt = maxoccur = umatchcnt = 0;
fprintf(stderr,"Writing index for %d-mers ...\n",pepsize);
binarray = toplevel[pepsize];
maxcode = pow20[pepsize];
for(sseqcode=0; sseqcode<maxcode; sseqcode++)
   if((binptr=binarray[sseqcode]) != NULL)
     // This pep has some matches
     {
     code2sseq(sseqcode, pepsize, subseq);
     idxpos = ftell(out);
     if(fprintf(out,"%s\n",subseq) <= 0)
       fprintf(stderr,"writing acs for %s failed\n",subseq);
     // write secundary index
     fprintf(out2,"%s %9u\n",subseq, idxpos);
     ac_cnt = iso_cnt = 0;
     usscnt++;
     while(binptr->nextbinptr != NULL)
          {
	  // add the next BINSIZE ac
	  for(i=0;i<BINSIZE;i++)
	     {
	     strcpy(ac,binptr->ac[i]);
	     fprintf(out,"%s\n",ac);
	     //fprintf(stderr,"ac: %s\n",ac);
	     iso_cnt++;
	     strcpy(ac_noiso,ac);
	     ac_noiso[6] = 0;
	     if(ac_noiso != lastac)
	       ac_cnt++;
	     strcpy(lastac,ac_noiso); 
	     }
	  // set pointer to next bin
	  binptr = binptr->nextbinptr;
	  }
     // dump last bin
     for(i=0;i<binptr->ac_cnt;i++)
	{
	strcpy(ac,binptr->ac[i]);
	fprintf(out,"%s\n",ac);
        iso_cnt++;
        strcpy(ac_noiso,ac);
        ac_noiso[6] = 0;
        if(ac_noiso != lastac)
	  ac_cnt++;
	strcpy(lastac,ac_noiso);
	}
	
     if(ac_cnt == 1)
       umatchcnt++;
     else if(ac_cnt > maxoccur)
       {
       maxoccur =  ac_cnt;
       strcpy(maxseq, subseq);
       }			    
     }
fprintf(stderr,"%d distincts %d-mer, %d are uniques (found in just 1 entry), most frequent: %s (%d)\n",usscnt, pepsize, umatchcnt, maxseq, maxoccur);
fclose(out);
fclose(out2);
}

// ---------------- pepx_saveall ---------------------
void pepx_saveall()
{
int i;
char fname[LINELEN];

for(i=MINPEPSIZE; i<= MAXPEPSIZE;i++)
   {
   if(!IL_merge)  
     sprintf(fname,"%s/pepx%d.idx",indexfolder,i);
   else
     sprintf(fname,"%s/pepxIL%d.idx",indexfolder,i);
   pepx_save(fname, i);
   }
}

/******* accompare **************************************************/  

int accompare (char *str1, char *str2)    
{
return (strncmp(str1,str2,6));
}

/******* compare **************************************************/  

int compare (char *str1, char *str2)    
{  
fseek(currentindex,(int)str2,0);
fgets(currentresult,32,currentindex);
//fprintf(stderr,"res: %s\n",currentresult);
return (strncmp(str1,currentresult,currentpepsize));
}

// ---------------- pepx_reportnomatch ---------------------
int pepx_reportnomatch(char* orgquerystring, char* querystring, char* outputmode)
{
if(!strcmp(outputmode,"BATCH"))
   fprintf(stdout,"\nNO_MATCH %s\n",orgquerystring);
else
   fprintf(stdout,"%s: No match\n",querystring);
return(0);
}

// ---------------- pepx_merge_with_prev_res ---------------------
int pepx_merge_with_prev_res(char endres[MAXSEQ][ACLEN], char curres[MAXSEQ][ACLEN], char* acstr, int rescnt)
{
// TODO: keep a better trace of variant matches, the info disappear when peptide extends too much after the variant site
char isoonly[ACLEN], *dashptr;  
int i,j;

//fprintf(stderr,"acstring: %s\n",acstr);
j = rescnt;
if(strlen(acstr) == 0) // first pep of a query
  for(i=0;i<rescnt;i++)
     {
     strcpy(endres[i],curres[i]);
     strcat(acstr,curres[i]);
     }
else // Check the match did exist for previous peps
  {
  for(i=j=0;i<rescnt;i++)
     {
     strcpy(isoonly,curres[i]);
     if((dashptr=strrchr(isoonly,'-')) != isoonly + 6)
       // Remove variant pos before searching prev results
       *dashptr = 0;
     //fprintf(stderr,"checking %s (result %d/%d)\n",isoonly,i+1,rescnt);
     if(strstr(acstr,isoonly))
       strcpy(endres[j++],curres[i]);
     }
     
  if(j)  
   // regenerate actring
   {
   *acstr = 0;
   for(i=0;i<j;i++)
    strcat(acstr,endres[i]);
   }
  }
return(j);
}

// ---------------- pepx_search ---------------------
int pepx_search(char* query, IDXDATA* idx)
{
typedef char actab[ACLEN];  
int i, j, jpos=-1, jcnt=0, found, rescnt=0, pepsize, fpos;
char querystring[MAXPEPSIZE], ac[ACLEN], newquery[MAXPEPSIZE], fname[LINELEN], buf[MAXPEPSIZE+1], *ptr;
char acholder[ACLEN+1];
actab *currentresults;
FILE* ffh;

strcpy(querystring,query);
pepsize = strlen(query);
currentpepsize = pepsize;
currentindex = idx[pepsize].fh;
if(idx==idxinfoIL)
  currentresults = resultsIL;
else
  currentresults = results;
for(i=0;i<pepsize;i++)
   if(querystring[i] == 'X')
     {
     jpos = i;
     jcnt++;
     }
   //else if(IL_merge && query[i] == 'L')
   else if((idx==idxinfoIL) && querystring[i] == 'L')
     querystring[i] = 'I';
     
if(jcnt > 1)
  // joker
  {
  fprintf(stderr,"\n%s: No more than 1 joker/6AA\n",query);
  return(0);
  }
else if((jcnt == 1) && (jpos != 0) && (jpos != pepsize-1))
  // internal joker
  {
  /*if((jpos == 0) || (jpos == pepsize-1))
    {
    fprintf(stderr,"\n%s: Joker must be internal\n",query);
    return(0);
    }*/
  for(i=0;i<strlen(aarevcode);i++)
     // For each of the 20 AAs
     {
     memset(newquery,0,MAXPEPSIZE);
     strncpy(newquery,querystring,jpos);
     strncat(newquery,&aarevcode[i],1);
     strcat(newquery,querystring + jpos + 1);
     //fprintf(stderr,"\nnewquery: %s\n",newquery);
     if(bsearch(newquery,NULL,idx[pepsize].elemcnt,pepsize+11,compare))
       {
       fpos = atoi(currentresult + pepsize);
       ffh = idx[pepsize].ffh;
       fseek(ffh,fpos,SEEK_SET);
       // skip peptide
       fgets(ac,ACLEN,ffh);
       while(fgets(ac,ACLEN,ffh))
            // get all ACs
            {
            if(strlen(ac) > 7)
              {
              if(ptr=strrchr(ac,'\n'))
	        *ptr = 0;
              else // max len identifiere eg:Q8WZ42-11-11658
                ac[ACLEN-1] = 0;
              // make results uniques
	      for(found=FALSE,j=0;j<rescnt;j++)
	         if(!strcmp(ac,currentresults[j]))
		   {
		   found = TRUE;
		   break;
		   }
	      if(!found)
                strcpy(currentresults[rescnt++],ac);
              }
            else
              break;
            }
       } 
     }
  return(rescnt);
  }

if(jpos == 0)
  // initial  joker -> reduce query length
  {
  strcpy(buf,querystring+1);
  strcpy(querystring,buf);
  currentpepsize = --pepsize;
  currentindex = idx[pepsize].fh;
  }
else if (jpos == pepsize-1)
  // terminal  joker -> reduce query length
  {
  querystring[pepsize-1] = 0;
  currentpepsize = --pepsize;
  currentindex = idx[pepsize].fh;
  }
  
// No jokers
if(!bsearch(querystring,NULL,idx[pepsize].elemcnt,pepsize+11,compare))
  // No match
  {
  //fprintf(stderr,"\n%s: not found in %s\n",querystring,idx==idxinfoIL?"ILindex":"BaseIndex");  
  return(0);
  }
//fprintf(stderr,"\n%s: found in %s\n",querystring,idx==idxinfoIL?"ILindex":"BaseIndex");
fpos = atoi(currentresult + pepsize);
ffh = idx[pepsize].ffh;
fseek(ffh,fpos,SEEK_SET);
// skip peptide
fgets(ac,ACLEN,ffh);
//fprintf(stderr,"\nReading match ACs for %s\n",ac);
while(fgets(acholder,ACLEN+1,ffh))
     // get all ACs
     {
     strncpy(ac,acholder,ACLEN);  
     if(strlen(ac) > 7)
       {
       if(ptr=strrchr(ac,'\n'))
	 *ptr = 0;
       else // max len identifiere eg:Q8WZ42-11-11658
         ac[ACLEN-1] = 0;
       //fprintf(stderr,"\nrescnt %d %s: %s(%d)\n",rescnt, query,ac,strlen(ac));
       strcpy(currentresults[rescnt++],ac);
       }
     else
       {
       //fprintf(stderr,"\nBreaking at %s\n",ac);	 
       break;
       }
     }
return(rescnt);
}

// ---------------- pepx_processquery ---------------------
int  pepx_processquery(char* orgquerystring)
{
char query[LINELEN], querystring[LINELEN], subquery[MAXPEPSIZE + 1]="", acstring[400000]="", finalres[MAXSEQ][ACLEN],acstringIL[400000]="", finalresIL[MAXSEQ][ACLEN];
char *qptr, ac[ACLEN];
int row=0, i, j, k, ac_cnt, cnt, cntIL, found;

// TODO: rewrite and iterate one by one AA, or 1rst and last and one by one ?
strcpy(query,orgquerystring);
if(!strcmp(outputmode,"BATCH"))
  // Only first token is the peptide
  {
  for(i=0;i<strlen(query);i++)
     if(isspace(query[i]))
       {
       query[i] = 0;
       break;
       }
  }

// quick filter
 for(i=0,qptr=querystring;i<strlen(query);i++)
   {
   // uppercase
   query[i] = toupper(query[i]);
   if(!strchr(aarevcode, query[i]))
     {
     if(isspace(query[i]) || isdigit(query[i])
     || (query[i] == '*')
     || (query[i] == '#')
     || (query[i] == '.')
     || (query[i] == '(')
     || (query[i] == ')'))
       // just don't copy unwanted, and allow to keep eventually tagged seq for result display
       i = i;
     else if(query[i] == 'X')
       *qptr++ = query[i];
     else
       {
       fprintf(stderr,"\n%c is not an AA\n",query[i]);
       return;
       }
     }
   else
     *qptr++ = query[i];
   }
*qptr=0;
strcpy(query,querystring);

  
while(strlen(query) > MAXPEPSIZE)
   { // Split query in overlaping subqueries of maximum length 
   strncpy(subquery,query,MAXPEPSIZE);
   subquery[MAXPEPSIZE] = 0;
   //fprintf(stderr,"subquery %s\n",subquery);
   strcpy(query,query + MAXPEPSIZE-1); // Prepare next subquery
   //fprintf(stderr,"will look for %s\n",query);
   if(IL_merge)
     {
     cntIL = pepx_search(subquery,idxinfoIL);
     //fprintf(stderr,"\n%s had %d IL matches\n",subquery,cntIL);
     if(cntIL == 0) // unmatched subquery
       return(pepx_reportnomatch(outputmode,querystring,orgquerystring));
     if(cntIL=pepx_merge_with_prev_res(finalresIL,resultsIL, acstringIL,cntIL) == 0)
       {
       //fprintf(stderr,"\nmerge failed for %s\n",subquery);	 
       return(pepx_reportnomatch(outputmode,querystring,orgquerystring));
       }
     }
   cnt=pepx_search(subquery,idxinfo);
   if((cnt==0) && !IL_merge)
     return(pepx_reportnomatch(outputmode,querystring,orgquerystring));     
   if((cnt=pepx_merge_with_prev_res(finalres, results, acstring,cnt) == 0) && !IL_merge)
     return(pepx_reportnomatch(outputmode,querystring,orgquerystring));     
   }
  
if(i = strlen(query)) // otherwise we're finished
  {
  //if(strlen(acstring)) // issue last subquery with the longest x-mer
  if(strlen(subquery)) // issue last subquery with the longest x-mer
    {
    strncpy(subquery, &querystring[strlen(querystring)-MAXPEPSIZE], MAXPEPSIZE);
    subquery[MAXPEPSIZE] = 0;
    }
  else // query peptide was <= MAXPEPSIZE
   strcpy(subquery,query);
  if(IL_merge)
    {
    cntIL = pepx_search(subquery,idxinfoIL);
    if(cntIL == 0)
      return(pepx_reportnomatch(outputmode,querystring,orgquerystring));
    if((cntIL=pepx_merge_with_prev_res(finalresIL, resultsIL, acstringIL,cntIL))==0)
      return(pepx_reportnomatch(outputmode,querystring,orgquerystring));
    
    }
  cnt = pepx_search(subquery,idxinfo);
  if((cnt==0) && !IL_merge)
    return(pepx_reportnomatch(outputmode,querystring,orgquerystring));     
    //fprintf(stderr,"last subquery %s gave %d matches\n",subquery,cnt);
  cnt=pepx_merge_with_prev_res(finalres, results, acstring,cnt);
  if((cnt == 0) && !IL_merge)
    return(pepx_reportnomatch(outputmode,querystring,orgquerystring)); 
  }

if(IL_merge && (cntIL != cnt))
  // Reccopy in finalres flaging the IL matches
  {
  for(i=k=0;i<cntIL;i++)
     {
     strcpy(ac,finalresIL[i]);
     for(j=found=0;j<cnt;j++)
        if(!strcmp(ac,finalres[j]))
	  {
	  found = 1;
	  break;  
	  }
     if(!found)
       // flag as IL
       {
       j = strrchr(ac,'-') - ac; 	 
       if(j < 8) // Don't combine IL anv variants for now
         {
         strcat(ac,"-IL");
         strcpy(finalres[cnt+k++],ac);
	 }
       }
     }
  //cnt = cntIL;   
  cnt += k;   
  }
  
// last subquery done: finish
if(!strcmp(matchmode,"ACONLY"))
   // Re-filter without iso ids
   {
   ac_cnt = 0;
   for(i=0;i<cnt;i++)
      {
      strcpy(ac,finalres[i]);
      *strchr(ac,'-') = 0;
      //fprintf(stderr,"ac=%s\n",ac);
      for(k=0,found=FALSE;k<ac_cnt;k++)
	 if(!strcmp(ac,results[k]))
	   found = TRUE;
      if(!found)
	strcpy(results[ac_cnt++],ac);
      }
   // re-fill final results
   cnt = ac_cnt;   
   for(i=0;i<cnt;i++)
      strcpy(finalres[i],results[i]);
   }  
   
if(!strcmp(outputmode,"BATCH"))
  {
  fprintf(stdout,"\n");
  for(i=0;i<cnt;i++)
     {
     fprintf(stdout,"%s",finalres[i]);
     if(i != cnt-1)
       fprintf(stdout,",");
     }
  if(cnt == 0)
    fprintf(stdout,"NO_MATCH");
  // Reminder of the query
  fprintf(stdout," %s\n",orgquerystring);
  }
else
  {
  fprintf(stdout,"\n%s: %d match(s)%s",querystring,cnt,linesep);
  for(i=0;i<cnt;i++)
     fprintf(stdout,"%s%s",finalres[i],linesep);
  if(cnt > 20)
    fprintf(stdout,"%s: %d match(s)%s",querystring,cnt,linesep);
  }
return(cnt);  
}

// ---------------- pepx_indexsubseq ---------------------
void pepx_indexsubseq(char* subseq, int varpos)
{
char id[ACLEN];
int i=0, pepsize, sseqcode;
BIN *binptr, *newbinptr, **binarray;

if(varpos) 
  sprintf(id,"%s-%d",currISO,varpos);
else
  strcpy(id,currISO);

pepsize = strlen(subseq);
if(IL_merge)
  // Replace all L with I
  for(i=0;i<pepsize;i++)
     if(subseq[i] == 'L')
       subseq[i] = 'I';

sseqcode = sscode(subseq);
binarray = toplevel[pepsize];
binptr = binarray[sseqcode];
if(binptr == NULL)
  // 1rst occurence of pep
  {
  binptr = malloc(sizeof(BIN));
  binptr->nextbinptr = NULL;
  binptr->ac_cnt = 1;
  strcpy(binptr->ac[0],id);
  // strcpy(binptr->ac[0],ACpos);  if we later record pos (would double index size)
  binarray[sseqcode] = binptr;
  }
else
  { 
  while(binptr->nextbinptr)
       // follow bins
       binptr = binptr->nextbinptr;
  if(binptr->ac_cnt == BINSIZE)
    {
    // get a new bin
    newbinptr = malloc(sizeof(BIN));
    binptr->nextbinptr = newbinptr;
    binptr = newbinptr;
    binptr->ac_cnt = 0;
    binptr->nextbinptr = NULL;
    //totalbins++;
    }
  i = binptr->ac_cnt++;
  strcpy(binptr->ac[i],id);
  //if(!strcmp(subseq,"AAAA"))  fprintf(stdout,"%s in %s now at %d\n",subseq,currAC,i);
  }
//if(!strcmp(subseq,"SRSDNA"))  fprintf(stdout,"%s in %s stored at pos %d in bin\n",subseq,currAC,i);
}

// ---------------- pepx_indexseq ---------------------
void pepx_indexseq(char* seq, int varcnt)
{
char subseq[MAXPEPSIZE+1], varsubseq[MAXPEPSIZE+1], variant[MAXVARINPEP][MAXPEPSIZE+1]; 
int i, k, varnb, too_many_variants = 0, seqlen, repeat_offset, varindex=1, pepsize;

seqlen = strlen(seq);
//printf("indexing %s\n",seq);
for(i=0;i<seqlen-MINPEPSIZE;i++)
   {
   // C-ters
   //printf("indexing at %d/%d\n",i,seqlen);
   for(pepsize=MINPEPSIZE;pepsize<=MAXPEPSIZE;pepsize++)
      {
      if(seqlen-i < pepsize)
        // subseq too short
        break;
      strncpy(subseq,seq+i,pepsize);
      subseq[pepsize] = 0;
      if(strchr(subseq,'X'))
        // Not indexable
	continue;
      // reference subseq is first in array
      strcpy(variant[0],subseq);
      strcpy(varsubseq,subseq);
      if(debug)
        if(pepsize == 6) fprintf(stderr,"%s at pos %d: %s\n",currAC,i,subseq);	
      if(varcnt)
	// This sequece has variants
	{
        for(k=0;k<pepsize;k++)
          // generate variant subseqs
          {
          strcpy(varsubseq,subseq);
          if(variants[i+k][0])
	    {
	    if(debug)
	      fprintf(stderr,"%s variant at pos %d is %c\n",currISO, i+k,variants[i+k][0]);
	    for(varnb = 0; varnb < 4; varnb++) 
	       {
	       if(variants[i+k][varnb] == 0)
		 break;
	       if(variants[i+k][varnb] == 'X')
	         // code for missing AA: shift by one and add next
	         {
	         if(k == 0)
	           // replace 1rst by previous
		   *(varsubseq+k) = seq[i-1];
	         else
	           // shift by one and add next
	           {
		   strncpy(varsubseq+k,varsubseq+k+1,pepsize-k);
		   *(varsubseq+pepsize-1) = seq[i+pepsize];
		   }
	         }
	       else
	         *(varsubseq+k) = variants[i+k][varnb];
	       strcpy(variant[varindex++],varsubseq);
 	       //printf("at varindex %d: %s\n",varindex-1, variant[varindex-1]);
	       if(varindex >= MAXVARINPEP)
	         {
	         //fprintf(stderr,"Enough variants for %s in %s\n",subseq,currAC);
	         too_many_variants = 1;
	         varindex_overflow_cnt++;
	         break; // enough
	         }
	       }
	    if(too_many_variants)
	      // Leave pepsize loop
	      {
	      too_many_variants = 0;
	      break;
	      }
	    }
	  }					   
	//printf("varindex loop for %s\n",subseq);
	}
      for(k=0;k<varindex;k++)
        // nonvariant subseq is 0
        {
	strcpy(subseq,variant[k]);
	//printf("varindex loop %d %s\n",k,subseq);
	if(k == 0)
	  // master seq
	  {
	  if(!strstr(seq+i+1,subseq))
            // ensures each ac is indexed only once per sscode
            pepx_indexsubseq(subseq,0); // else  fprintf(stdout,"repeated pep: %s\n",subseq);
	  }
	else if(strlen(subseq) == pepsize && !strstr(masterseq,subseq))
	   // variant peps must not exist elsewhere in master
	   {
	   //fprintf(stderr,"Variant subseq: %s at pos %d in %s\n",subseq,i+1,currAC);  
	   pepx_indexsubseq(subseq,i+1);
	   }
            //else{printf("i:%d k:%d: orgsseq:%s(%d) sseq:%s\n",i,k,variant[0],pepsize,subseq);}
	}
      }
   // Reset varindex for next peptide
   varindex = 1;
   }
}

// ---------------- pepx_build ---------------------
void pepx_build(char* seqfilename)
{
FILE *in;
char fname[LINELEN], varaa, *ptr, *varptr, varbuf[16], buf[MAXSEQSIZE];
int seqcnt=0, currVarcnt, i, seqlen;

/* if((varfh=fopen("NextProtvariants","w"))==NULL)
 {
 perror("NextProtvariants");
 exit(2);
 }
*/
if((in=fopen(seqfilename,"r"))==NULL)
 {
 perror(seqfilename);
 exit(2);
 }

if(!ignore_variants)
  fprintf(stdout,"Indexing sequences with NextProt variants, please wait...\n");
else
  fprintf(stdout,"Indexing sequences without variants, please wait...\n");
if(IL_merge)
  fprintf(stdout,"Indexing with I/L merged...\n");
pepx_initindexes();
fgets(buf,LINELEN,in); // Skip header line if any
if(strstr(buf,"NX_"))
  // There was no header
  rewind(in);
while(fgets(buf,MAXSEQSIZE,in))
    {
    strncpy(currISO,buf+3,12);
    *strchr(currISO,'\t') = 0;
    strncpy(currAC,currISO,6);
    currAC[6]=0;
    //if(seqcnt > 7000)
      //fprintf(stderr,"%s\n",currISO);
    strcpy(masterseq,strrchr(buf,'\t')+1);
    *strrchr(masterseq,'\n')=0;
    seqlen = strlen(masterseq);
    if(IL_merge)
      // Replace all L with I
      for(i=0;i<seqlen;i++)
         if(masterseq[i] == 'L')
            masterseq[i] = 'I';

    if(!strcmp(currAC,"Pxxxxx"))
    //if(!strcmp(currAC,"P00533"))
      {
      debug=TRUE;
      fprintf(stderr,"input buffer: %s...",buf);
      }
    else			
      debug=FALSE;		  		      
      //debug=TRUE;		  		      
    // get variants for current entry
    if(!ignore_variants)
       currVarcnt = pepx_build_varindex(currISO);
    else
       currVarcnt = 0;
    totalvarcnt += currVarcnt;
    pepx_indexseq(masterseq,currVarcnt);
    seqcnt++;
    if((seqcnt % 1000) == 0)
      fprintf(stderr,"Processing seq %d...\n",seqcnt);
    }
fprintf(stderr,"%d sequences indexed (%d simple variants, %d 1-miss variants)\n",seqcnt,totalvarcnt,varmisscnt);
fprintf(stderr,"%d varindex overflows\n",varindex_overflow_cnt);
if(!debug)
  pepx_saveall();
if(varfh)
  fclose(varfh);
}

// ---------------- printHelp ---------------------
void  printHelp(char *mode)
{
if(!strcmp(mode,"ARGS"))
  {
  fprintf(stderr,"\nPepx possible arguments are:\n\n");
  fprintf(stderr,"--search (short=-s) to perform a peptide search\n");
  fprintf(stderr,"--build (short=-b) to build indexes (requires the isoform-file name as mandatory argument)\n");
  fprintf(stderr,"--version (short=-v) to show current version\n");
  fprintf(stderr,"--help (short=-h) to show this help\n");
  fprintf(stderr,"--index-folder (short=-x) to specify an index folder (default is .)\n");
  fprintf(stderr,"--variant-folder (short=-w) to specify a folder for json variants (required for build command when ignore-variants flag is not set)\n");
  fprintf(stderr,"--rest-url (short=-r) REST server to retrieve json variants when variant folder is empty (for 1rst build with a given variant folder)\n");
  fprintf(stderr,"--peptide-file (short=-p) a file with peptides to search (1 peptide/line, if not provided peptides will be read from stdin)\n");
  fprintf(stderr,"--ignore-variants to build indexes not considering variants\n");
  fprintf(stderr,"--IL to build indexes merging I and L\n");
  fprintf(stderr,"--noiso (short=-n) to output search results at the entry level\n");
  }
fprintf(stderr,"\nCurrent limitation:\n\n");
fprintf(stderr,"- poly-AA stretches > %d cannot be found\n",MAXPEPSIZE);
fprintf(stderr,"- only snp-style (1 AA for 1 other AA), and 1-AA-miss variants are accounted \n");
fprintf(stderr,"- only 32 variant accounted within a given x-mer\n");
fprintf(stderr,"- only 1 joker (X) allowed in a given x-mer\n"); 
}

// ---------------- main ---------------------
int main(int argc, char **argv)
{
char *ptr, seqfname[LINELEN]="", command[LINELEN], querystring[LINELEN];
char query[LINELEN], pepfname[LINELEN]="";
int i, c;
int option_index = 0; // getopt_long stores the option index here.
FILE* inputstream = stdin;

static struct option long_options[] = {
               {"ignore-variants",   no_argument,       &ignore_variants, 1},
               {"IL",   no_argument,       &IL_merge, 1},
               {"search",  no_argument,       0, 's'},
               {"noiso",   no_argument,       0, 'n'},
               {"help",   no_argument,       0, 'h'},
               {"version",   no_argument,       0, 'v'},
               {"build", required_argument, 0, 'b'},
               {"peptide-file", required_argument, 0, 'p'},
               {"variant-folder",  required_argument, 0, 'w'},
               {"rest-url",  required_argument, 0, 'r'},
               {"index-folder",required_argument, 0, 'x'},
               {0, 0, 0, 0}
             };
// Further improvements: Rewrite the code in scala to take advantage of powerful hashmap-like data structures
// Consider indexing non-snp vatiants (alleles...)

if(argc < 2)
 {
 if((ptr=getenv("QUERY_STRING")) == NULL)
   {
   fprintf(stderr,"Usage: maxindex [build or search or search-noiso] [filename or peptide or INTERACTIVE (if search)]\n");
   fprintf(stderr,"if no peptide is given as second arg, then they are read from stdin\n\n");
   exit(1);
   }
 // for web cgi
 strcpy(envstring,ptr);
 fprintf(stdout,"Content-type: text/html\n\n");
 if(ptr=strstr(envstring,"pep="))
   {
   strcpy(command,"search");
   strcpy(querystring,ptr+4);
   strcpy(linesep,"<br>");
   if(ptr=strstr(envstring,"=noiso"))
      // output matches at the entry level
      strcpy(matchmode,"ACONLY");
   }
 else
   {
   fprintf(stdout,"envstring: %s\n",envstring);
   fprintf(stdout,"No valid arguments...\n");
   exit(0);
   }
 }
else
  // non-web usage: parse command arguments
  {
  while((c = getopt_long (argc, argv, "snhvb:p:f:w:r:x:", long_options, &option_index)) != -1)
        {
       switch (c)
             {
             case 0:
               break;
     
             case 'n':
               strcpy(matchmode,"ACONLY"); // For search command only
               break;
     
             case 'v':
	       printf ("Current version is `%s'\n", version);
               exit(0);
               
             case 'h':
               printHelp("ARGS");
               exit(0);
     
             case 's':
	       strcpy(command,"search");
               break;
               
             case 'b':
               strcpy(command,"build");
	       strcpy(seqfname,optarg);
               break;
     
             case 'p':
	       strcpy(pepfname,optarg);
	       if((inputstream=fopen(pepfname,"r"))==NULL)
	         {
		 perror(pepfname);
	         exit(6);
		 }
               break;
     
             case 'w':
               strcpy(varfolder,optarg);
               break;
     
             case 'r':
               strcpy(RESTserver, optarg);
               break;
     
             case 'x':
               strcpy(indexfolder, optarg);
               break;
     
             default:
	       printf("%c: Unknown option...\n", c);
               abort();
             }

       }
  }

if(!strcmp(command,"build"))
  {
  if(strlen(seqfname) == 0)
    {
    fprintf(stderr,"'build' requires a sequence file for argument\n");
    exit(2);
    }
  if ((ignore_variants == 0) && strlen(varfolder) == 0)  
    {
    fprintf(stderr,"'build' with variants requires a variant folder for argument (option -v)\n");
    exit(2);
    }
  start = clock();
  //strcpy(seqfname,argv[2]);
  printf("Building with seqfile %s, indexes at %s, variants at %s\n",seqfname,indexfolder,ignore_variants?"ignored":varfolder);
  //exit(0);
  pepx_build(seqfname);
  fprintf(stderr, "Duration: %d\n", clock() - start);
  }
else if(!strncmp(command,"search",6))
  {
  //if(argc > 2)
    //strcpy(querystring,argv[2]);
  if(optind < argc)
    strcpy(querystring,argv[optind]);
  else if(strlen(envstring) == 0)
    strcpy(querystring,"BATCH");
  printf("Searching for %s, indexes at %s\n",querystring,indexfolder);
  //exit(0);
  pepx_loadall();
  if(!strcmp(querystring,"INTERACTIVE") || !strcmp(querystring,"BATCH"))
    {
    strcpy(outputmode,querystring);
    printHelp("");
    do
     {
     if(!strcmp(querystring,"INTERACTIVE"))
       printf("\nEnter or paste peptide query (spaces and digits will be skipped)? ");
     // get user input
     if(!fgets(query,LINELEN,inputstream))
       break;
     if(ptr=strrchr(query,'\n'))
       *ptr=0;
     if(strlen(query) < 2)
       exit(0);
     pepx_processquery(query);
     }
    while(TRUE);
    }
  else
     // Just process given query
     pepx_processquery(querystring);
  }
else
  {
  fprintf(stderr,"command arg must be either 'build' or 'search'\n");
  exit(4);
  }
return(0);
}


