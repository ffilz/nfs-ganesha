(* STATS.PAS

This program generates a cost and level table for characteristic potions.
3 sections are printed:

   1. Normal Magic User
   2. Normal Cleric (vigor)
   3. Strong Cleric

This is a new version that handles the new MP cost spells with the following
conditions:

- continuing cost is always 2/hr/point
- all stats are linear cost
- costs are 2 MP/point, 1.5 MP/point, and 1 MP/point
- levels are +0, +2, +4 respectively
- Vigor is 7th level, clerics get it at 6th
- Unlike cast spells, clerical potions are assumed to have the same initial
  cost as mage spells (the cast spells are 1 SP/point at the first level the
  spell appears).

*)

Unit stats;

Interface

Uses Defs, Costs, LDefs, SPList;

procedure DoStatList;

{----------------------------------------------------------------------------------}
Implementation

const
   HighestLevel = 20;
   HighestPlus  = 20;
   Numreduced = 3;
   NumDurations = 2;
   NumDex = 5;
   NumDexP = 4;

type
   a1 = array[1..NumReduced] of word;
   da = array[1..NumDurations] of integer;
   idx = array[1..NumDex] of integer;
   rdx = array[1..NumDex] of integer;
   RomanNumeralArray = array[1..20] of string[5];
   StatTypes = (StatSTR, StatCON, StatDEX16, StatDEX18, StatDEX20, StatDEX23, StatDEX26);
   DexP = array[StatDEX16..StatDex26,0..NumDexP] of integer;
   CommonArray = array[StatTypes,
                       1..NumDurations,
                       6..HighestLevel,
                       1..HighestPlus] of word;

const
   Dexes : DexP = ((5,2,4,5,99),  { worthwhile pluses for StatDEX16 }
                   (6,2,4,6,99),  { worthwhile pluses for StatDEX18 }
                   (7,2,4,6,7),   { worthwhile pluses for StatDEX20 }
                   (8,3,5,7,8),   { worthwhile pluses for StatDEX23 }
                   (9,3,6,8,9));  { worthwhile pluses for StatDEX26 }
   ima : a1 = (1,   3,   1);  { numerator for MP cost multiplier   }
   ida : a1 = (1,   4,   2);  { denominator for MP cost multiplier }
   {          x1 x3/4 x1/2 }
   lp  : a1 = (0,   2,   4);  { spell level adder for cheaper spells }
   ShortestDuration : da = (  120,  30);
   LongestDuration  : da = (32767, 120);

var
   ComCost : CommonArray;
   ComMP   : CommonArray;
   ComLP   : CommonArray;
   ComInit : CommonArray;
   ComNeed : CommonArray;
   Common  : integer;
   NR      : integer;

const
   Ignore = $FFFF;
   StatHeader : string = 'Potion             MP  Cost Duration    NOMORE';
   StatMask   : string = 'llllllllllll LLLLL RRR RRRR LLLLLLLLLLLL';
   StatHPos   = 1;
   StatHLen   = 19;
   StatWidth  = 40;
   roman : RomanNumeralArray =
           ('I','II','III','IV','V','VI','VII','VIII','IX','X',
            'XI','XII','XIII','XIV','XV','XVI','XVII','XVIII','XIX','XX');

{ Compute duration of potion given a plus, initial MP and total MP.
  This is simple since the continuing cost is always 2 MP/point/hour.
}
Function Duration(Plus, InitMP, PotionMP : Integer) : real;
begin
   Duration := 60.0 * (PotionMP - InitMP) / 2 / Plus;
end;

Function MinDuration(Plus, InitMp, Shortest : integer) : integer;
Var D : integer;
begin
   D := InitMP + Shortest * Plus * 2 div 60;
   while ((D - InitMP) * 60 / (Plus * 2)) < Shortest do Inc(D);
   MinDuration := D;
end;

function MaxPlus(LowL, Delta : integer) : integer;
var mi, mp : integer;
begin
   mi := NumReduced;
   if Delta = 0 then
      while AML < (LowL + lp[mi]) do dec(mi);
   mp := AML * ida[mi] div (ima[mi] * 2);
   if mp > HighestPlus
      then MaxPlus := HighestPlus
      else MaxPlus := mp;
end;

procedure FillCommon(LowL, Plus, Delta : integer;
                     Type1             : StatTypes;
                     Dur1              : integer);
var ca, ka, mpa  : a1;
    mi, i, j, li : integer;
begin
   if (AML + Delta) < LowL then exit;
   mi := NumReduced;
   if Delta = 0 then
      while AML < (LowL + lp[mi]) do dec(mi);
   if Delta = 0
      then li := 1
      else li := mi;
   for i := li to mi do
      begin
      ka[i] := (2 * Plus * ima[i] + ida[i] - 1) div ida[i];
      if ka[i] > AML
         then mpa[i] := $FFFF
         else mpa[i] := MinDuration(Plus, ka[i], ShortestDuration[Dur1]);
      if mpa[i] < AML
         then mpa[i] := AML;
      if mpa[i] > 12 * AML
         then ca[i] := $FFFF
         else
            begin
            if Delta = 0
               then Pot(AML,mpa[i],5,10,sr[5+AML-LowL-lp[i]])
               else Pot(AML,mpa[i],5,10,sr[5+AML-LowL+Delta]);
            ca[i] := trunc(cost);
            end;
      end;
   if Delta = 0
      then
         begin
         j := 1;
         for i := 2 to mi do
            if ca[i] < ca[j]
               then j := i;
         end
      else j := mi;

   if Duration(Plus, ka[j], mpa[j]) > LongestDuration[Dur1]
      then exit;

   ComCost[Type1,Dur1,AML,Plus] := ca[j];
   ComMP[  Type1,Dur1,AML,Plus] := mpa[j];
   ComInit[Type1,Dur1,AML,Plus] := ka[j];
   ComLP[  Type1,Dur1,AML,Plus] := lp[j];
end;

procedure FindNeed(LowL, Plus, Delta : integer;
                   Type1 : StatTypes;
                   Dur1 : integer);
var
    i, k : integer;
    d    : real;
    t, t2 : StatTypes;
begin
   if ComCost[Type1, Dur1, AML, Plus] = $FFFF
      then exit;
   if Plus > 5
      then d := ComCost[Type1, Dur1, AML, Plus] * 1.5
      else d := ComCost[Type1, Dur1, AML, Plus] * 1.2;

   if Type1 >= StatDEX16
      then t2 := StatDEX26
      else t2 := Type1;

   if AML > common
      then for i := 6 to common do
         for k := 1 to Dur1 do
            for t := Type1 to t2 do
               if ComCost[t,k,i,Plus] < d
                  then exit;

   for k := 1 to Dur1-1 do
      for t := Type1 to t2 do
         if ComCost[t,k,AML,Plus] < d
            then exit;

   if (Plus = HighestPlus) or
      (ComNeed[Type1,Dur1,AML,Plus+1] = 0)
      then ComNeed[Type1,Dur1,AML,Plus] := 1
      else
         begin
         k := Plus + 1;
         while (k <= HighestPlus) and
               (ComNeed[Type1,Dur1,AML,k] = 0)
            do inc(k);
         if (k > HighestPlus) or
            (ComCost[Type1, Dur1, AML, k] > d)
            then ComNeed[Type1,Dur1,AML,Plus] := 1;
         end;
end;

procedure CheapStat(s1 : string;
                    LowL, Plus, Delta : integer;
                    Type1 : StatTypes;
                    Dur1 : integer);
var
    d             : real;
    mp, init, lp1 : word;
begin
   if ComNeed[Type1, Dur1, AML, Plus] = 0
      then exit;

   mp   := ComMP[  Type1, Dur1, AML, Plus];
   init := ComInit[Type1, Dur1, AML, Plus];
   lp1  := ComLP[  Type1, Dur1, AML, Plus];
   d    := Duration(Plus, init, mp);

   inc(NR);
   Cells[NR, 1] := s1;
   Cells[NR, 2] := Roman[LowL+lp1];
   Cells[NR, 3] := strv(mp);
   Cells[NR, 4] := strv(5+AML-LowL-lp1+Delta);
   Cells[NR, 5] := '+' + strv(Plus) + PTimeMins(' for ',d,1,'');
   VPotion(NR);
end;

procedure AllStats(DeltaStr, DeltaCon, DeltaDex : integer);
var DoStr, DoCon, DoDex : boolean;
   procedure ChSt(s1 : string;
                  LowL, Delta : integer;
                  type1 : StatTypes);
   var Plus, MaxP, Dur1 : integer;
   begin
      if (AML + Delta) < LowL
         then exit;
      MaxP := MaxPlus(LowL, Delta);
      for Plus := 1 to MaxP do
         for Dur1 := 1 to NumDurations do
            FillCommon(LowL,Plus,delta,type1,dur1);
      for Plus := MaxP downto 1 do
         for Dur1 := 1 to NumDurations do
            FindNeed(LowL,Plus,Delta,Type1,Dur1);
      for Plus := 1 to MaxP do
         for Dur1 := 1 to NumDurations do
            CheapStat(s1,LowL,Plus,delta,type1,dur1);
   end;
   procedure ChDx(s1 : string;
                  LowL, Delta : integer;
                  type1 : StatTypes);
   var Plus, MaxP, Dur1, i : integer;
   begin
      if (AML + Delta) < LowL
         then exit;
      if Dexes[Type1,0] > MaxPlus(LowL, Delta)
         then MaxP := MaxPlus(LowL, Delta)
         else MaxP := Dexes[Type1,0];

      i := 1;
      while (i < NumDexP) and (Dexes[Type1, i] <= MaxP) do
         begin
         Plus := Dexes[Type1, i];
         for Dur1 := 1 to NumDurations do
            FillCommon(LowL,Plus,delta,type1,dur1);
         inc(i);
         end;
      if (MaxP < Dexes[Type1,0]) and (MaxP > Dexes[Type1, i-1]) then
         for Dur1 := 1 to NumDurations do
            FillCommon(LowL,MaxP,delta,type1,dur1);

      for Plus := MaxP downto 1 do
         for Dur1 := 1 to NumDurations do
            FindNeed(LowL,Plus,Delta,Type1,Dur1);
      for Plus := 1 to MaxP do
         for Dur1 := 1 to NumDurations do
            CheapStat(s1,LowL,Plus,delta,type1,dur1);
   end;
begin
   DoStr := DeltaStr >= 0;
   DoCon := DeltaCon >= 0;
   DoDex := DeltaDex >= 0;
   if DoDex then ChDx('Coordination 26', 16, DeltaDex, StatDEX26);
   if DoDex then ChDx('Coordination 23', 13, DeltaDex, StatDEX23);
   if DoDex then ChDx('Coordination 20', 10, DeltaDex, StatDEX20);
   if DoDex then ChDx('Coordination 18',  8, DeltaDex, StatDEX18);
   if DoDex then ChDx('Coordination 16',  6, DeltaDex, StatDEX16);
   if DoStr then ChSt('Strength',         3, DeltaStr, StatSTR);
   if DoCon then ChSt('Vigor',            7, DeltaCon, StatCON);
end;

procedure GenStatPotions(Com, SAml                    : integer;
                         s1                           : string;
                         DeltaStr, DeltaCon, DeltaDex : integer);
var Rows : integer;
begin
   Common := Com;
   FillChar(ComCost, sizeof(ComCost), $FF);
   FillChar(ComNeed, sizeof(ComNeed), 0);
   AML := SAml;
   while AML <= MaxLevel do
      begin
      NR := 0;
      AllStats(DeltaStr, DeltaCon, DeltaDex);
      Rows := FinishCells(NR);
      ShowList(Rows, 1, s1 + ' LEVEL ' + StrV(AML), true);
      inc(AML);
      end;
end;

procedure DoStatList;
begin
   GetOutFile(2);
   GetParameterInt(MaxLevel,3);
   if MaxLevel > HighestLevel
      then MaxLevel := HighestLevel;
   PutStar('*TITLE=STANDARD CHARACTERISTIC POTIONS','');
   PutStar('*TITLE=H2','');
   PutStar('*COLUMNS=3','');
   PutStar('*WIDTH=' + strv(StatWidth),'');
   PutStar('*LABEL=H3','');
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
   GenStatPotions(8,6,'MAGE',0,0,0);
   {
   GenStatPotions(8,6,'CLERIC (NORMAL)',-1,1,-1);
   GenStatPotions(8,6,'CLERIC (STRONG)',1,2,1);
   }
   EndDoc;
   close(fout);
end;

Begin
   HelpText[HelpIndex] := 'RUN T {outfile} {max lev}                               stat potions list';
   inc(HelpIndex);
   CallProcs[callindex] := DoStatList;
   CallChars[callindex] := 'T';
   inc(callindex);
End.
