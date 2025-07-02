(* COSTS.PAS
*)
unit Costs;

interface

uses Defs, RTFOut;

const
   lowsr = -3;
   HighestLevel = 20;
   maxsr = 25;

type
   ItemTypes = (ItemNone, ItemTargeted, SelfTargeted, SuckOnIt, Targeted, SpellTransfer);
   IntegerPerItemType = array[ItemTypes] of integer;
   lary = array[0..HighestLevel] of real;
   ilary = array[0..HighestLevel] of integer;
   srary = array[-23..25] of real;
   iary = array[0..12] of integer;

const
   ItemType : ItemTypes = ItemTargeted;
   ChargesMinLevel    : IntegerPerItemType = (0,   8,    9,   10,   11,   99);
   PermMinLevel       : IntegerPerItemType = (0,   9,   10,   11,   13,    9);
   PermMaxSpellLevel  : IntegerPerItemType = (0,  -2,   -4,   -5,   -6,    0);
   Concentration : ilary = (0,0,0,0,0,0,0,0,1,2,2,3,3,3,4,4,4,4,5,5,5);
   enchantmentmp : iary = (1,2,4,6,10,16,24,34,52,76,116,176,256); {values x 2}

   pfocus : array[0..20] of byte = (0,0,0,0,0,3,5,6,8,9,11,12,14,15,17,18,20,21,23,24,26);

   sr : srary = (0.0, 0.0, 0.0, 0.0013, 0.0022, 0.0035, 0.0054, 0.0082, 0.012,
                0.018, 0.025, 0.036, 0.049, 0.067, 0.088, 0.12, 0.15, 0.18,
                0.23, 0.27, 0.33, 0.38, 0.44,
                0.50,
                0.56, 0.62, 0.67, 0.73, 0.77, 0.82, 0.85, 0.88,
                0.912, 0.933, 0.951, 0.964, 0.975, 0.982, 0.988, 0.9918,
                0.9946, 0.9965, 0.9978, 0.9987, 0.99918, 0.99952, 0.99972,
                0.99984, 0.999912);

   basesr = 0.77; {SR[5]}


   SPDivisor  : integer = 21;  { old = 28 }
   levelto : integer = 3;
   incomemult : real = 1.5;
   MaxLevel : integer = 14;
   MaxInt   : integer = 26;
   MinInt   : integer = 8;
   msr : integer = 15;
   ItemTypeNames : array[ItemTypes] of string[16] = (
           'NONE',
           'ITEM TARGETED',
           'SELF TARGETED',
           'SUCK-ON-IT',
           'TARGETED (WAND)',
           'SPELL TRANSFER');

var
   AML                  : integer;
   Time, Cost, MPCost   : real; { the various cost formulas generate these as side effects }
   CLChg, MUChg, MUMake : lary;
   mppday               : ilary;
   pr2, pr              : srary;
   pv                   : array[5..25,-23..25] of real;

function ValStr(value : real; columns, roundto : integer) : string;

function PotV(AML, MP : integer; csr : real) : real;

function pot(AML, MP, flen, roundto : integer; csr : real) : string;

function Charge(ityp : ItemTypes; AML, MP, flen, roundto : integer) : string;

procedure PermanentV(ityp : ItemTypes; AML, MP, SP, SpellLevel : integer;
                     var MPCost, SPCost, SpellCost, MPTime, SPTime, SpellTime : real);

function Enchantment(Plus, AML, EffLevel : integer; Hm : boolean) : real;

function StorerV(AML, MP : integer) : real;

function GrowerV(AML, MP : integer) : real;

function UsedPrice(ityp : ItemTypes; MP, OrigSR, CurrSR, AML : integer) : real;

{ ptimedays:
  Display value in days, months, or years to fit columns.
}
function PTimeDays(value : real; columns : integer) : string;


{ ptimemins:
  Display value in minutes or hours to fit columns.
}
function PTimeMins(s1 : string; value : real; columns : integer; s2 : string) : string;

type
   spFunc = function(AML, int : integer) : real;

function SpellPoints(Level, Wisdom : integer) : real;
function MageMemorization(AML, int : integer) : real;
function ClericMemorization(AML, wis : integer) : real;

{----------------------------------------------------------------------------------}
implementation

const
   ChargesDays        : IntegerPerItemType = (0,  170,  340,  510,  680,    0);
   ChargesMaterials   : IntegerPerItemType = (0,   20,   40,   60,   80,    0);
   PermDaysMP         : IntegerPerItemType = (0, 1700, 1700, 1700, 1700,  170);
   PermDaysSpellLevel : IntegerPerItemType = (0,  850, 1700, 2550, 3400,  170);
   PermMaterialsMP    : IntegerPerItemType = (0,  400,  600,  900, 1400,    0);
   PermMaterialsSpl   : IntegerPerItemType = (0,  200,  600, 1350, 2800,    0);
   spmul = 1.2;                              {for permanent items}

{ compute the number of spell points for a cleric }
function SpellPoints(Level, Wisdom : integer) : real;
begin
   case Level of
      1 :        SpellPoints := 0.6 * SpellPoints(3,Wisdom);
      2 :        SpellPoints := 0.8 * SpellPoints(3,Wisdom);
      3..32767 : SpellPoints := Level / SPDivisor * Wisdom * (Level + 1);
      else       SpellPoints := 0;
      end;
end;

function MageMemorization(AML, int : integer) : real;
begin
   MageMemorization := 1 + AML * AML / 72 * int * (AML + 6.2);
   {                                               old = 6.0 }
end;

function ClericMemorization(AML, wis : integer) : real;
begin
   ClericMemorization := 1.5 * MageMemorization(AML,wis);
end;

function ValStr(value : real; columns, roundto : integer) : string;
var s : string;
    rv : longint;
    col : integer;
begin
   col := columns;
   rv := round(value/roundto) * roundto;
   str(rv,s);
   if length(s) > col then begin
      str(round(value/1000),s);
      s := s + 'k';
      if length(s) > col then begin
         str(round(value/1.0e6),s);
         s := s + 'm';
         if length(s) > col then
            begin
            s := '************';
            SetLength(s, col);
            end;
         end;
      end;
   ValStr := s;
end;

function PTimeDays(value : real; columns : integer) : string;
var s : string;
    r1 : real;
    rv : longint;
    c : char;
    col : integer;
begin
   col := columns - 1;
   r1 := value;
   if frac(r1) > 0 then r1 := r1 + 1.0;
   rv := trunc(r1);
   str(rv,s);
   c := 'd';
   if length(s) > col then begin
      str(round(rv/7),s);
      c := 'w';
      if length(s) > col then begin
         str(round(rv/30),s);
         c := 'm';
         if length(s) > col then begin
            str(round(rv/360),s);
            c := 'y';
            if length(s) > col then begin
               s := '************';
               c := '*';
               SetLength(s, col);
               end;
            end;
         end;
      end;
   PTimeDays := s + c;
end;

function PTimeMins(s1 : string; value : real; columns : integer; s2 : string) : string;
var s : string;
    r1 : real;
    rv : longint;
    c : char;
    col : integer;
begin
   col := columns - length(s1) - length(s2) - 1;
   r1 := value * 12;
   if frac(r1) > 0 then r1 := r1 + 1.0;
   rv := trunc(r1);
   str(rv:col,s);
   c := 't';
   if rv > 5 then begin
      str(round(rv/12),s);
      c := 'm';
      if (col > 1) and (length(s) > col) then begin
         str(round(rv/720),s);
         c := 'h';
         if length(s) > col then begin
            s := '************';
            c := '*';
            SetLength(s, col);
            end;
         end;
      end;
   ptimemins := s1 + s + c + s2;
end;

function PotV(AML, MP : integer; csr : real) : real;
var MinDaysForMP : real;
begin
   MPCost       := 50 / AML * MP;
   Time         :=  8 / AML * MP / AML;
   MinDaysForMP := MPCost/mppday[AML];

   if MinDaysForMP > Time
      then Time := MinDaysForMP;

   { in the following formula the 20 comes from the cost of a potion blank }
   Cost := (MPCost * muchg[AML] + Time * mumake[AML] + 20.0) / csr;

   PotV := Cost;
end;

function Pot(AML, MP, FLen, RoundTo : integer; csr : real) : string;
begin
   Pot := ValStr(PotV(AML, MP, csr), FLen, RoundTo);
end;

function ChargeV(ityp : ItemTypes; AML, MP : integer) : real;
var MinDaysForMP : real;
begin
   Time    := ChargesDays[ityp]/(AML * AML)/(AML + 1) * MP;
   MinDaysForMP := MP/mppday[AML];

   if MinDaysForMP > Time
      then Time := MinDaysForMP;

   Cost := MP * muchg[AML] + Time * mumake[AML] + ChargesMaterials[ityp];
   ChargeV := Cost;
end;

function Charge(ityp : ItemTypes; AML, MP, FLen, RoundTo : integer) : string;
begin
   Charge := ValStr(ChargeV(ityp, AML, MP), FLen, RoundTo);
end;

procedure PermanentV(ityp : ItemTypes; AML, MP, SP, SpellLevel : integer;
                     var MPCost, SPCost, SpellCost, MPTime, SPTime, SpellTime : real);
var Level2 : real;
begin
   Level2    := AML * AML;
   MPTime    := PermDaysMP[ityp]  / Level2 * MP;
   SPTime    := PermDaysMP[ityp]  / Level2 * SP * spmul;
   SpellTime := PermDaysSpellLevel[ityp] / Level2 * SpellLevel;
   MPCost    := (muchg[AML] + PermMaterialsMP[ityp]        ) * MP + MPTime    * mumake[AML];
   SPCost    := (clchg[AML] + PermMaterialsMP[ityp] * spmul) * MP + SPTime    * mumake[AML];
   SpellCost :=               PermMaterialsSpl[ityp]         * MP + SpellTime * mumake[AML];
   Time      := MPTime + SPTime + SpellTime;
   Cost      := MPCost + SPCost + SpellCost;
end;

function Enchantment(Plus, AML, EffLevel : integer; Hm : boolean) : real;
var MP, Eff, MinDaysForMP : real;
begin
   case AML + EffLevel of
      1..9   : Eff := 1.0;
      10..12 : Eff := 2/3;
      13..16 : Eff := 0.5;
      else     Eff := 0.4;
      end;
   if Hm
      then MP := (EnchantmentMP[plus*2] + EnchantmentMP[plus*2-1])
      else MP := EnchantmentMP[plus];
   MP           := MP * Eff;
   Time         := MP *  20 / AML;
   MPCost       := MP * 400 / AML;
   MinDaysForMP := MPCost/mppday[AML];

   if MinDaysForMP > Time
      then Time := MinDaysForMP;

   Enchantment := Time * mumake[AML] + MPCost * muchg[AML];
end;

function StorerV(AML, MP : integer) : real;
var MinDaysForMP : real;
begin
   MPCost       := 600/AML * MP;
   Time         :=  18/AML * MP;
   MinDaysForMP := MPCost/mppday[AML];

   if MinDaysForMP > Time
      then Time := MinDaysForMP;

   StorerV := MPCost * muchg[AML] + Time * mumake[AML] + 100 * MP;
end;

function GrowerV(AML, MP : integer) : real;
var MinDaysForMP : real;
begin
   MPCost       := 10080/AML * MP;
   Time         :=   960/AML * MP;
   MinDaysForMP := MPCost/mppday[AML];

   if MinDaysForMP > Time
      then Time := MinDaysForMP;

   GrowerV := MPCost * muchg[AML] + Time * mumake[AML];
end;

function UsedPrice(ityp : ItemTypes; MP, OrigSR, CurrSR, AML : integer) : real;
var BaseCost : real;
begin
   BaseCost := ChargeV(ityp, AML, MP);
   if pv[OrigSR, CurrSR] > 0.06
      then Cost := BaseCost * pv[OrigSR, CurrSR]
      else Cost := BaseCost * 0.06;
   UsedPrice := Cost;
end;

procedure InitValues;
var i, j :  integer;
begin
   for i := 2 to HighestLevel         do mppday[i] := mppday[i-1] + 8;
   for i := 2 to levelto              do mumake[i] := mumake[1];
   for i := levelto+2 to HighestLevel do mumake[i] := mumake[i-1] * incomemult;
   for i := 1 to HighestLevel         do muchg[i]  := mumake[i] / mppday[i];
   for i := 1 to  5                   do clchg[i]  := mumake[i] / (2 * SpellPoints(i,15+i));
   for i := 6 to HighestLevel         do clchg[i]  := mumake[i] / (2 * SpellPoints(i,20));
   fillchar(pr,sizeof(pr),0);
   fillchar(pr2,sizeof(pr2),0);
   fillchar(pv,sizeof(pv),0);
   { calculate expected value of successeful uses of a charged item
     roll 1d6 each use of item: 1) down 0, 2) down 1, 3-4) down 2, 5-6) down 3
     the 1 in 6 chance of going down 0 results in an expected value of .2
     uses before it goes down, so the number of successefull uses at a given
     SR is 1.2 times the probability of success. Then you get the expected
     value of successeful uses for whatever SR the item drops to. There are three
     possibilities of what SR the item ends up at, but one is half the probability
     of the other, so it happens 1 in 5 given that the SR drops, the other two
     happen 2 in 5. So the expected value there is (a + b*2 + c*2)/5 or
     .4 * (a/2 + b + c) where a, b, and c are the expected values for SR-1, SR-2,
     and SR-3 respectively.
     The expected values are calculated for number of uses before item drops to
     SR -4 or less (at which time it's close to useless) and to SR -21 or less
     (at which time it vaporizes). The expected values for the SRs below this
     cutoff are thus 0, which gives us a starting point for the calculations.
   }
   for i := lowsr-3 to lowsr-1
      do pr[i] := 0.0;
   for i := lowsr to maxsr
      do pr[i] := 0.4 * (pr[i-1] / 2.0 + pr[i-2] + pr[i-3]) + sr[i] * 1.2;
   for i := -23 to -21
      do pr2[i] := 0.0;
   for i := -20 to maxsr
      do pr2[i] := 0.4 * (pr2[i-1] / 2.0 + pr2[i-2] + pr2[i-3]) + sr[i] * 1.2;
   for j := 5 to msr
      do for i := lowsr to maxsr
         do pv[j,i] := pr[i] / pr[j];
end;

Begin
   mumake[1] := 4.0;
   mppday[1] := 96;
   mumake[4] := 10.0;
   InitValues;
End.
