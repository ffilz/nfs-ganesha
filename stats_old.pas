(* STATS_OLD.PAS

This program generates a cost and level table for characteristic potions.
4 sections are printed:

   1. Normal Magic User
   2. Strong Cleric
   3. Faery Magic User
   4. Strong Faery Cleric

The Faery spells are Coordination for Elves and Constitution for Dwarves.

*)

Unit stats_old;

Interface

Uses Defs, Costs, LDefs, SPList;

procedure DoStatList_Old;

{----------------------------------------------------------------------------------}
Implementation

const
   HighestLevel = 20;
   HighestPlus1 = 20;
   HighestPlus  = 21;

const
   NumReduced = 4;

type
   a1 = array[1..NumReduced] of word;

const
   ima : a1 = (1,   2,   1,   2);  { numerator for MP cost multiplier   }
   ida : a1 = (1,   3,   2,   5);  { denominator for MP cost multiplier }
   {          x1 x2/3 x1/2 x2/5 }
   lp  : a1 = (0,   2,   5,   9);  { spell level adder for cheaper spells }

type
   RomanNumeralArray = array[1..20] of string[5];
   StatTypes = (StatSTR, StatCON, StatDEX16, StatDEX18, StatDEX20, StatDEX23, StatDEX26);

var
   Cmn : array[StatTypes,1..3,6..HighestLevel,0..HighestPlus1,1..HighestPlus] of word;
   {                     dur1 }
   Common : integer;
   InitDiv : integer;
   NR : integer;

const
   SType = 1;
   CType = 2;
   DType = 3; {Delta Plus}
   Ignore = $FFFF;
   StatHeader : string = 'Potion              MP Cost Duration    NOMORE';
   StatMask   : string = 'lllllllllllll LLLLL RR RRRR LLLLLLLLLLLL';
   StatHPos   = 1;
   StatHLen   = 19;
   StatWidth  = 40;
   roman : RomanNumeralArray =
           ('I','II','III','IV','V','VI','VII','VIII','IX','X',
            'XI','XII','XIII','XIV','XV','XVI','XVII','XVIII','XIX','XX');

Function Duration(InitMp, PotionMP : Integer) : real;
Var Init : integer;
begin
   Init := (InitMP + InitDiv - 1) div InitDiv;
   Duration := (PotionMP - Init) * InitDiv * 60 / InitMP;
end;

Function MinDuration(InitMp, Minutes : integer) : integer;
Var Init, D : integer;
begin
   Init := (InitMP + InitDiv - 1) div InitDiv;
   D := Init + Minutes * InitMP div (60 * InitDiv);
   {if initmp = 0 then writeln('Plus=',plus,' Plus1=',plus1);}
   while ((D - Init) * InitDiv * 60 / InitMP) < Minutes do Inc(D);
   MinDuration := D;
end;

{ Calculate mp cost of +p1 to +p stat of styp }
function CalcMP(p1, p, STyp : integer) : integer;
begin
   case STyp of
      SType : calcmp := p * 2;
      CType : calcmp := p * (p + 1) div 2;
      DType : calcmp := (p * (p + 1) div 2) - (p1 * (p1 + 1) div 2);
      else    calcmp := 0;
      end;
end;

{ Determine maximum plus possible }
function MaxPlus(Plus1, InitMP, InitDiv, STyp, MaxP : integer) : integer;
var i : integer;
begin
   case STyp of
      SType : i := AML;
      CType : i := trunc(sqrt(AML * 8 + 1)) - 1;
      DType : i := trunc(sqrt((AML + CalcMP(0, Plus1, DType)) * 8 + 1)) - 1;
      else    i := 0;
      end;
   i := i * InitDiv div (InitMP * 2);
   if i > MaxP
      then MaxPlus := MaxP
      else MaxPlus := i;
end;


procedure CheapStat(s1 : string;
                    styp, lowl, plus1, plus, da, db, delta : integer;
                    type1 : StatTypes;
                    dur1 : integer);
var ca       : array[1..4] of word;
    ka, mpa  : a1;
    mi, i, j : integer;
    d        : real;
    c        : word;
    sa       : array[1..4] of string;
    Ignore   : boolean;
   procedure checkplus;
   var i, j, p1 : integer;
   begin
      for i := 6 to common do
         for j := 1 to dur1 do
            for p1 := 0 to plus1 do
               if cmn[type1,j,i,p1,plus] < d
                  then
                     begin
                     Ignore := true;
                     exit;
                     end;
      for j := 1 to dur1 do
         for p1 := 0 to plus1-1 do
            if cmn[type1,j,AML,p1,plus] <= c
               then
                  begin
                  Ignore := true;
                  exit;
                  end;
   end;
begin
   Ignore := false;
   if (AML + delta) < lowl then exit;
   mi := NumReduced;
   while (AML + delta) < (lowl + lp[mi]) do dec(mi);
   for i := 1 to mi do begin
      InitDiv := ida[i];
      ka[i] := calcmp(Plus1, Plus, STyp) * ima[i];
      if ((ka[i] + InitDiv - 1) div InitDiv) > AML
         then mpa[i] := $FFFF
         else mpa[i] := MinDuration(ka[i], da);
      if mpa[i] < AML then mpa[i] := AML;
      if mpa[i] > 12 * AML
         then ca[i] := $FFFF
         else
            begin
            sa[i] := pot(AML,mpa[i],5,10,sr[5+AML-lowl-lp[i]+delta]);
            ca[i] := trunc(cost);
            end;
      end;
   j := 1;
   c := ca[1];
   for i := 2 to mi do if ca[i] < c then
      begin
      j := i;
      c := ca[j];
      end;
   d := 1.1 * c;
   if styp = DType
      then checkplus
      else if AML > common then
         for i := 6 to common do
            {for k := 1 to dur1 do}
               if cmn[type1,dur1,i,plus1,plus] < d
                  then Ignore := true;
   cmn[type1,dur1,AML,plus1,plus] := c;
   InitDiv := ida[j];
   d := Duration(ka[j],mpa[j]);
   if (not Ignore) and (d < db) then
      begin
      inc(NR);
      Cells[NR, 1] := s1;
      Cells[NR, 2] := Roman[lowl+lp[j]];
      Cells[NR, 3] := strv(mpa[j]);
      Cells[NR, 4] := strv(5+AML-lowl-lp[j]+delta);
      if styp = DType
         then Cells[NR, 5] := '+' + strv(plus1) + '>+' + strv(plus) + PTimeMins(',',d,1,'')
         else Cells[NR, 5] := '+' + strv(plus) + PTimeMins(' for ',d,1,'');
      VPotion(NR);
      end
end;

procedure allstats(DoStr, DoCon, DoCoord : boolean; delta : integer; s1 : string);
var MaxPS, MaxPC : integer;
   procedure chst(s1 : string;
                  styp, lowl, da, db, delta, maxp : integer;
                  type1 : StatTypes;
                  dur1 : integer);
   var Plus, Plus1 : integer;
   begin
      if (styp = DType)
         then Plus1 := 1
         else Plus1 := 0;
      Plus := Plus1 + 1;
      while Plus1 <= MaxP do
         begin
         while Plus <= MaxP do
            begin
            CheapStat(s1,styp,lowl,Plus1, Plus,da,db,delta,type1,dur1);
            inc(Plus);
            end;
         if (styp = DType)
            then
               begin
               inc(Plus1);
               Plus := Plus1 + 1;
               end
            else Plus1 := MaxP + 1;
         end;
   end;
begin
   MaxPS := MaxPlus(0,2,5,SType,HighestPlus);
   MaxPC := MaxPlus(0,2,5,CType,HighestPlus);
   if DoStr   then chst('Strength',     SType, 3, 30,32767,delta,MaxPS,StatSTR,  1);
   if DoStr   then chst('Strength',     SType, 3, 60,  120,delta,MaxPS,StatSTR,  2);
   if DoCon   then chst('Constitution', CType, 8, 30,32767,delta,MaxPC,StatCON,  1);
   if DoCon   then chst('Constitution', DType, 8, 30,32767,delta,MaxPC,StatCON,  1);
   if DoCon   then chst('Constitution', CType, 8, 60,  120,delta,MaxPC,StatCON,  2);
   if DoCon   then chst('Constitution', DType, 8, 60,  120,delta,MaxPC,StatCON,  2);
   if DoCoord then chst(s1+'Coord 16',  CType, 6, 30,32767,delta,    5,StatDEX16,1);
   if DoCoord then chst(s1+'Coord 16',  DType, 6, 30,32767,delta,    5,StatDEX16,1);
   if DoCoord then chst(s1+'Coord 16',  CType, 6, 60,   60,delta,    5,StatDEX16,2);
   if DoCoord then chst(s1+'Coord 16',  DType, 6, 60,   60,delta,    5,StatDEX16,2);
   if DoCoord then chst(s1+'Coord 18',  CType, 8, 30,32767,delta,    6,StatDEX18,1);
   if DoCoord then chst(s1+'Coord 18',  DType, 8, 30,32767,delta,    6,StatDEX18,1);
   if DoCoord then chst(s1+'Coord 18',  CType, 8, 60,   60,delta,    6,StatDEX18,2);
   if DoCoord then chst(s1+'Coord 18',  DType, 8, 60,   60,delta,    6,StatDEX18,2);
   if DoCoord then chst(s1+'Coord 20',  CType,10, 30,32767,delta,    7,StatDEX20,1);
   if DoCoord then chst(s1+'Coord 20',  DType,10, 30,32767,delta,    7,StatDEX20,1);
   if DoCoord then chst(s1+'Coord 20',  CType,10, 60,   60,delta,    7,StatDEX20,2);
   if DoCoord then chst(s1+'Coord 20',  DType,10, 60,   60,delta,    7,StatDEX20,2);
   if DoCoord then chst(s1+'Coord 23',  CType,13, 30,32767,delta,    8,StatDEX23,1);
   if DoCoord then chst(s1+'Coord 23',  DType,13, 30,32767,delta,    8,StatDEX23,1);
   if DoCoord then chst(s1+'Coord 23',  CType,13, 60,   60,delta,    8,StatDEX23,2);
   if DoCoord then chst(s1+'Coord 23',  DType,13, 60,   60,delta,    8,StatDEX23,2);
end;

procedure GenStatPotions(Com, SAml             : integer;
                         s1                    : string;
                         DoStr, DoCon, DoCoord : boolean;
                         Delta                 : integer;
                         s2                    : string);
var Rows : integer;
begin
   Common := Com;
   fillchar(cmn,sizeof(cmn),$FF);
   AML := SAml;
   while AML <= MaxLevel do
      begin
      NR := 0;
      AllStats(DoStr, DoCon, DoCoord, Delta, s2);
      Rows := FinishCells(NR);
      ShowList(Rows, 1, s1 + ' LEVEL ' + StrV(AML), true);
      inc(AML);
      end;
end;

procedure getnext(var s : string);
begin
end;

procedure DoStatList_Old;
begin
   GetOutFile(2);
   if MaxLevel > HighestLevel
      then MaxLevel := HighestLevel;
   PutStar('*TITLE=STANDARD CHARACTERISTIC POTIONS','');
   PutStar('*TITLE=CENTERED','');
   PutStar('*COLUMNS=3','');
   PutStar('*WIDTH=' + strv(StatWidth),'');
   PutStar('*LABEL=CENTER','');
   PutStar('*HPOS=' + strv(StatHPos),'');
   PutStar('*HLEN=' + strv(StatHLen),'');
   PutStar('*TYPE=POTION','');
   PutStar('*IDENTSTRING=''''','');
   PutStar('*MASK=', StatMask);
   PutStar('*MPCOL=3','');
   PutStar('*MPLEN=2','');
   PutStar('*SRCOL=4','');
   PutStar('*SRLEN=4','');
   PutStar('*COSTCOL=4','');
   PutStar('*COSTLEN=4','');
   PutStar('*HEADER=', StatHeader);
   StartDoc;
   PrintTitle;
   GetCellText(0, StatHeader);
   GenStatPotions(7,6,'MAGE',true,true,true,0,'');
   GenStatPotions(7,6,'CLERIC',true,true,true,1,'');
   EndDoc;
   {
   GenStatPotions(0,6,'ELF MAGE',false,false,true,2,'Elf ');
   GenStatPotions(7,6,'DWARF MAGE',false,true,false,2,'');
   GenStatPotions(7,6,'ELF CLERIC',false,false,true,3,'Elf ');
   GenStatPotions(7,6,'DWARF CLERIC',false,true,false,3,'');
   }
   close(fout);
end;

Begin
   HelpText[HelpIndex] := 'RUN O {outfile}                                         old stat potions list';
   inc(HelpIndex);
   CallProcs[callindex] := DoStatList_Old;
   CallChars[callindex] := 'O';
   inc(callindex);
End.
