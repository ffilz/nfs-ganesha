(* SPLIST.PAS
*)

unit SpList;

interface

(*
SPELL LIST DOCUMENTATION

anything in all caps MUST be in all caps.

{position}       one of LEFT, RIGHT or CENTER
{direction}      one of UP, DOWN, UPVALUE, DOWNVALUE

*TITLE=sssssssss         must be before start of data to be included
*TITLE={position}        format of title
*FOOTER={position}       format of following footers
*FOOTER=sssssssss        must be before start of data there can be up to 10
                         lines of footers
*FOOTER=CANCEL           cancels existing footers
*COLUMNS=xx              number of data columns
*WIDTH=xx                width of data column
*HEADER=NONE             no header in the next section
*HEADER=                 header must be on next line
ssssssssssssssssss
*HPOS=xx                 position in label in header
*HLEN=xx                 maximum length of label
*LABEL={position}        if header then label in header
*LABEL=HEADER            place LABEL in header
*LABEL=NONE              no label in the next section
*SRCOL=xx                column that contains SR for items
*SRLEN=xx                length of SR output strings
*SR*POS=xx               position of multiple SRs
*COSTCOL=xx              column that contains COST for items
*COSTLEN=xx              length of cost output strings
*MPCOL=xx                column that contains MP for items
*MPLEN=xx                length of MP output strings
*MKLCOL=xx               Column to supply AML for item
*MKLLEN=xx               length of AML output strings
*PLUSCOL=xx              Column that contains the plus of an item
*PLUSLEN=xx              length of plus in output strings
*SPLCOL=xx               Column that contains the spell level of an item
*SPLLEN=XX               length of spell level output strings
*CLERIC                  set items to be made by clerics
*MAGE                    set items to be made by mages

                         Note on the LEN items, these are used in output only,
                         so can be different from the mask length of the column.

*LEVEL=xx                set AML for items
*PCONC=ON or OFF         turn on or off use of concentration for potions if
                         it will save money.
*IDENTSTRING='ssssss'    string to place in identicals
*MASK=                   mark out the columns for output
LLLL CCCCC RRRRR JJJJJ llll cccc rrrr jjjj
                         LLL = left aligned column
                         CCC = center aligned column
                         RRR = right aligned column
                         JJJ = justified column
                         lower case = identical entries in this column should
                                      be blanked out
*PMASK=                  mark out columns for input, must follow *MASK
XXXXX XXXX XXXX          if it is used. Any non-blank marks part of a column.
                         This allows the input text to be broken up on a different
                         column width than the output.
*SORTCOLS=xx1,xx2,...    columns of sort fields
*SORTDIR:xx={direction}  sort direction, for sort x, VALUE types sort by value
                         allowing left justified numbers
*SORTTYPE=QUICK          use quicksort (uses stack)
*SORTTYPE=SHELL          use shell sort (iterative)
*SORTTYPE=OFF            no sorting
*TYPE=SPELL              produce spell lists
*TYPE=POTION             produce potion lists
*TYPE=SPELL TRANSFER     spell transfer lists
*TYPE=ENCHANTMENT        enchantment lists, the + is in the MP column
*TYPE=STORER             storer lists
*TYPE=GROWER             grower lists
*TYPE=ITEM TARGETTED     item lists \
*TYPE=SELF TARGETTED     item lists | followed by CHARGED or PERMANENT
*TYPE=SUCK-ON-IT         item lists | CHARGED is assumed default
*TYPE=TARGETTED          item lists /
*TYPE=OTHER              other magic items
*TEXT                    start a text block
ENDTEXT                  end a text block
*anything else           a comment
^Lanything               a comment w/form feed of .IN file
a label                  1st item after commands is the label, always required
                         even if there are no labels
a spell                  spells may be any thing
another spell
END                      end of section (required except last one)
another section label
a spell
END                      last end of section may be ommitted
END.                     end of file ( may be ommitted)
*)

uses Defs, Costs, LDefs, RTFOut;

procedure DoSpellList;

procedure PutStar(s1, s2 : string);

procedure VPotion(Row : integer);

{----------------------------------------------------------------------------------}
implementation

const
   SRCol   : integer = 0;
   SRLen   : integer = 1;
   CostCol : integer = 0;
   CostLen : integer = 1;
   TimeCol : integer = 0;
   TimeLen : integer = 1;
   NoteCol : integer = 0;
   MPCol   : integer = 0;
   MPLen   : integer = 1;
   MKLCol  : integer = 0;
   MKLLen  : integer = 1;
   SplCol  : integer = 0;
   SplLen  : integer = 1;
   PlusCol : integer = 0;
   PlusLen : integer = 1;
   srstarpos : integer = 1;
   IsCleric = 0;
   IsMage   = 1;
   MadeBy : integer = IsMage;

var
   CurrentLine  : string;   { current line being parsed }
   CurrentLine2 : string;   { next line when text internaly supplied }

{ test if CurrentLine starts with s1 }
function sscomp(s1 : string) : boolean;
var s2 : string;
begin
   s2 := copy(CurrentLine, 1, length(s1));
   sscomp := s1 = s2;
end;

{ if global string begins with s1, then extract integer v assuming it
  is within the range of min and max (inclusive) }
procedure Value(s1 : string; var v : integer; PosEq, Min, Max : integer);
var error, t : integer;
begin
   if SSComp(s1) then begin
      t := ExtractInt(CurrentLine,PosEq+1,127,error);
      if (error = 0) and (t >= min) and (t <= max) then v := t;
      end;
end;

{ extract a list of values for an item (format ITEM=1,2,3) }
procedure Values(s1 : string; var Ints : longintarray; PosEq :  integer; var NumI : integer);
var i : integer;
begin
   if SSComp(s1) then
      begin
      ExtractInts(CurrentLine,PosEq+1,127,ints,numi);
      write(NumI, ' Values of ', s1, ' are ');
      for i := 1 to NumI do
         begin
         write(Ints[i]);
         if i < NumI then write(', ');
         end;
      writeln;
      end;
end;

{ Get centering for an item (format ITEM=CENTER) }
function GetPosition(labl : string; var pos : integer) : boolean;
begin
   GetPosition := false;
        if SSComp(labl+'=CENTER') then pos := CenterField
   else if SSComp(labl+'=LEFT')   then pos := LeftField
   else if SSComp(labl+'=RIGHT')  then pos := RightField
   else if SSComp(labl+'=H1')     then pos := StyleH1
   else if SSComp(labl+'=H2')     then pos := StyleH2
   else if SSComp(labl+'=H3')     then pos := StyleH3
   else if SSComp(labl+'=H4')     then pos := StyleH4
   else if SSComp(labl+'=H5')     then pos := StyleH5
   else if SSComp(labl+'=NORM')   then pos := StyleNorm
   else if SSComp(labl+'=BODY')   then pos := StyleBody
   else if SSComp(labl+'=NONE')   then pos := NoHeader
   else GetPosition := true;
end;

{ compare s1[pos1, len] against s2[pos1, len] using comparison of type ctyp,
  which can be string or numeric compare }
function SComp(s1, s2 : string; pos : byte) : boolean;
begin
   SComp := copy(s1, pos, length(s2)) = s2;
end;

procedure ReadStar(IsFile : boolean);
var
   PosQuote, snum, err, PosEq : integer;
begin
   PosEq := pos('=',CurrentLine);
   PosQuote := pos('''',CurrentLine);
   if SSComp('*TYPE=') then begin
      ItemType := ItemNone;
      ptyp := 0;
      if SSComp('*TYPE=SPELL')          then ptyp := SpellsType;
      if SSComp('*TYPE=POTION')         then ptyp := PotionType;
      if SSComp('*TYPE=ITEM TARGETED')  then ItemType := ItemTargeted;
      if SSComp('*TYPE=SELF TARGETED')  then ItemType := SelfTargeted;
      if SSComp('*TYPE=SUCK-ON-IT')     then ItemType := SuckOnIt;
      if SSComp('*TYPE=TARGETED')       then ItemType := Targeted;
      if SSComp('*TYPE=SPELL TRANSFER') then ItemType := SpellTransfer;
      if SSComp('*TYPE=STORE')          then ptyp := StorerType;
      if SSComp('*TYPE=GROW')           then ptyp := GrowerType;
      if SSComp('*TYPE=ENCH')           then ptyp := EnchantmentType;
      if SSComp('*TYPE=OTHER')          then ptyp := OtherType;
      if pos('CHARGE',CurrentLine) > 0 then ptyp := ChargedType;
      if (pos('PERM',CurrentLine) > 0) or (ItemType = SpellTransfer) then ptyp := PermanentType;
      if (ItemType <> ItemNone) and (ptyp = 0) then ptyp := ChargedType;
      end;
   if SSComp('*STYLE=') then
      if GetPosition('*STYLE', Style)
         then writeln('Style must be followed by a style name.');
   if SSComp('*TITLE=') then
      if GetPosition('*TITLE',titlepos) then begin
         title := copy(CurrentLine,PosEq+1,127);
         newtitle := true;
         end;
   if SSComp('*FOOTER=') then
      begin
      writeln('Footer line ignored:');
      writeln(CurrentLine);
      end;
   if SSComp('*PCONC') then pconc := pos('ON',CurrentLine) > 0;
   value('*WIDTH',width,PosEq,10,127);
   value('*COLUMNS',columns,PosEq,1,10);
   value('*HPOS',hpos,PosEq,1,127);
   value('*HLEN',hlen,PosEq,1,127);
   value('*SRCOL',SRCol,PosEq,0,MaxCols);
   value('*SRLEN',SRLen,PosEq,1,127);
   value('*SR*POS',srstarpos,PosEq,0,127);
   value('*COSTCOL',CostCol,PosEq,0,MaxCols);
   value('*COSTLEN',CostLen,PosEq,1,127);
   value('*MPCOL',MPCol,PosEq,0,MaxCols);
   value('*MPLEN',MPLen,PosEq,1,127);
   value('*SPLCOL',SPLCol,PosEq,0,MaxCols);
   value('*SPLLEN',SPLLen,PosEq,1,127);
   value('*MKLCOL',MKLCol,PosEq,0,MaxCols);
   value('*MKLLEN',MKLLen,PosEq,1,127);
   value('*TIMECOL',TimeCol,PosEq,0,MaxCols);
   value('*TIMELEN',TimeLen,PosEq,1,127);
   value('*PLUSCOL',PlusCol,PosEq,0,MaxCols);
   value('*PLUSLEN',PlusLen,PosEq,1,127);
   value('*NOTECOL',NoteCol,PosEq,0,MaxCols);
   value('*LEVEL',AML,PosEq,1,HighestLevel);
   value('*LINES',NumRowsLine,PosEq,0,1000);
   values('*SORTCOLS=',SortCols,PosEq,NumSorts);
   if SSComp('*SORTDIR:') then begin
      {if NumSorts1 < NumSorts then NumSorts := NumSorts1;
      if NumSorts < NumSorts1 then NumSorts1 := NumSorts;}
      snum := ExtractInt(CurrentLine,8,PosEq-1,err);
      if (err = 0) and (snum > 0) and (snum <= NumSorts) then begin
         if SComp(CurrentLine,'=UP'   ,PosEq) then SortDir[snum] := StringGreater;
         if SComp(CurrentLine,'=DOWN' ,PosEq) then SortDir[snum] := StringLess;
         if SComp(CurrentLine,'=UPV'  ,PosEq) then SortDir[snum] := ValueGreater;
         if SComp(CurrentLine,'=DOWNV',PosEq) then SortDir[snum] := ValueLess;
         end;
      writeln('Sort dir # ',  snum,' for column ', SortCols[snum], ' is ', SortDir[snum]);
      end;
   if SSComp('*CLERIC') then MadeBy := IsCleric;
   if SSComp('*MAGE') then MadeBy := IsMage;
   if SSComp('*SORTTYPE=Q') then SortType := QuickSortType;
   if SSComp('*SORTTYPE=S') then SortType := ShellSortType;
   if SSComp('*SORTTYPE=B') then SortType := BubbleSortType;
   if SSComp('*SORTTYPE=OFF') then NumSorts := 0;
   if SSComp('*TEXT') then textmode := true;
   if SSComp('*ENDTEXT') or
      SSComp('*ETX') then textmode := false;
   if SSComp('*IDENTSTRING') then
      identstring := copy(CurrentLine,PosQuote+1,length(CurrentLine)-1-PosQuote);
   if SSComp('*HEADER=') then
      if SSComp('*HEADER=NONE') then hdr := NoHeader
      else begin
      if IsFile then
         if eof(fin) then
            begin
            writeln('ERROR - no header provided, no change');
            exit;
            end;
      if IsFile then readln(fin,CurrentLine2);
      header := MakeField(CurrentLine2,width,LeftField);
      hdr := InHeader;
      end;
   if SSComp('*MASK=') then
      begin
      if IsFile
         then
         if eof(fin) then
            begin
            writeln('ERROR - no mask provided, no change');
            exit;
            end;
      if IsFile then readln(fin,CurrentLine2);
      CellMask := MakeField(CurrentLine2,width,LeftField);
      FindCells;
      end;
   if SSComp('*PMASK=') then
      begin
      if IsFile
         then
         if eof(fin) then
            begin
            writeln('ERROR - no mask provided, no change');
            exit;
            end;
      if IsFile then readln(fin,CurrentLine2);
      PositionMask := CurrentLine2 + ' L';
      FindPositions;
      end;
   if SSComp('*LABEL=') then
      if GetPosition('*LABEL',lbl)
         then if SSComp('*LABEL=HEADER')
            then lbl := InHeader;
end;

procedure PutStar(s1, s2 : string);
begin
   CurrentLine  := s1;
   CurrentLine2 := s2;
   ReadStar(false);
end;

function ReadS : boolean;
var cmd : boolean;
    i   : integer;
begin
   i := 0;
   repeat
      if eof(fin)
         then CurrentLine := 'END.'
         else readln(fin,CurrentLine);
      cmd := CurrentLine[1] = '*';
      if cmd
         then
         begin
         inc(i);
         ReadStar(true);
         end;
   until not cmd;
   ReadS := i <> 0;
end;

procedure InsertCost(Row : integer; s1 : string);
begin
   if CostCol <> 0
      then Cells[Row, CostCol] := s1;
end;

procedure InsertTime(Row : integer; s1 : string);
begin
   if TimeCol <> 0
      then Cells[Row, TimeCol] := s1;
end;

procedure InsertSR(Row : integer; s1 : string);
begin
   if SRCol <> 0
      then Cells[Row, SRCol] := s1;
end;

procedure InsertMP(Row : integer; s1 : string);
begin
   if MPCol <> 0
      then Cells[Row, MPCol] := s1;
end;

procedure VPotion(Row : integer);
var vs1, vs2          : string;
    c1, srr, t1, t2   : real;
    mp, i, err, NumSR : integer;
begin
   mp := GetInt(Row, MPCol, err);
   if err <> 0 then begin
      end
   else begin
      srs[1] := GetInt(Row, SRCol, err);
      NumSR := 1;
      if err <> 0 then ExtractInts(CurrentLine,SRStarPos,127,srs,NumSR);
      srr := 1.0;
      for i := 1 to NumSR do srr := srr * sr[srs[i]];
      vs1 := pot(AML,mp,costlen,10,srr);
      t1 := Time;
      c1 := Cost;
      srr := 1.0;
      for i := 1 to NumSR do srr := srr * sr[srs[i]+Concentration[AML]];
      vs2 := pot(AML,mp,costlen,10,srr);
      t2  := Time;
      if pconc and (Cost < c1) then
         begin
         vs1 := vs2;
         t1  := t2;
         end;
      InsertCost(Row, vs1);
      InsertTime(Row, PTimeDays(t1, TimeLen));
      end;
end;

procedure VCharge(var Row : integer);
var v1, v2  : real;
    mp, err : integer;
begin
   mp := GetInt(Row, MPCol, err);
   if err <> 0
      then
         begin
            if pos('CONCENTRATION',CurrentLine) = 1
               then
                  begin
                  InsertCost(Row, '+'+ValStr(mumake[AML],costlen-1,1));
                  InsertSR  (Row, '+'+ValStr(Concentration[AML],srlen-1,1));
                  InsertTime(Row, '+1d');
                  end
         end
      else
         begin
         srs[1] := GetInt(Row, SRCol, err);
         InsertCost(Row, charge(ItemType,AML,mp,costlen,10));
         InsertTime(Row, PTimeDays(Time, TimeLen));
         v2 := cost + mumake[AML];
         v1 := UsedPrice(ItemType,mp,srs[1],srs[1]+Concentration[AML],AML);
         if pos('w/CONC',CurrentLine) > 0
            then
               begin
               if v2 > v1 then v1 := v2;
               InsertCost(Row, ValStr(v1,costlen,10));
               InsertSR  (Row, ValStr(srs[1]+Concentration[AML],srlen,1));
               InsertTime(Row, PTimeDays(Time+1, TimeLen));
               end
         end;
end;

procedure VEnchantment(var Row : integer);
var Plus, EffL, err : integer;
begin
   Plus := GetInt(Row, PlusCol, err);
   if MadeBy = IsCleric
      then EffL := 1
      else EffL := 0;
   if Plus > (AML - 8 + EffL)
      then
         begin
         InsertCost(Row, 'N/A');
         InsertTime(Row, 'N/A');
         InsertMP(Row, 'N/A');
         end
      else
         begin
         InsertCost(Row, ValStr(Enchantment(Plus, AML, EffL, false), CostLen, 50));
         InsertTime(Row, PTimeDays(Time, TimeLen));
         InsertMP(Row, ValStr(MPCost, MPLen, 1));
         end;
end;

procedure VPermanent(var Row : integer);
var mp, spl, sp, err : integer;
    MPCost, SPCost, SpellCost, MPTime, SPTime, SpellTime : real;
begin
   sp  := 0;
   mp  := GetInt(Row, MPCol, err);
   if MadeBy = IsCleric then
      begin
      sp := mp;
      mp := 0;
      end;
   spl := GetInt(Row, SPLCol, err);
   PermanentV(ItemType, AML, MP, SP, Spl,
              MPCost, SPCost, SpellCost, MPTime, SPTime, SpellTime);
   InsertCost(Row, ValStr(Cost, CostLen, 100));
   InsertTime(Row, PTimeDays(Time, TimeLen));
end;

procedure VStorer(var Row : integer);
begin
end;

procedure VGrower(var Row : integer);
begin
end;

procedure VOther(var Row : integer);
var MP, err : integer;
    MinTime : real;
begin
   MP := GetInt(Row, MPCol, err);
   Time := GetTimeDays(Row, TimeCol, err);
   MinTime := MP/MPPDay[AML];
   if MinTime > Time then Time := MinTime;
   Cost := MP * muchg[AML] + Time * mumake[AML];
   InsertCost(Row, ValStr(Cost, CostLen, 10));
   InsertTime(Row, PTimeDays(Time, TimeLen));
end;

function ReadAll : integer;
var n, err : integer;
begin
   n := 0;
   while not SSComp('END') do
      begin
      inc(n);
      GetCellText(n, CurrentLine);
      if MKLCol <> 0 then AML := GetInt(n, MKLCol, err);
      case PTyp of
         SpellsType      : ;
         ChargedType     : VCharge(n);
         PotionType      : VPotion(n);
         EnchantmentType : VEnchantment(n);
         PermanentType   : VPermanent(n);
         GrowerType      : VGrower(n);
         StorerType      : VStorer(n);
         OtherType       : VOther(n);
         end;
      ReadS;                end;
   ReadAll := FinishCells(n);
end;

procedure ShowText;
var spc, cmd, period : boolean;
begin
   while TextMode do
      begin
         StartPara(Style, 10, 10, false);
         spc := false;
         cmd := false;
         period := false;
         while (CurrentLine <> '') and (not cmd) do
            begin
            if spc then write(fout, ' ');
            if period then write(fout, ' ');
            writeln(fout, CurrentLine);
            period := CurrentLine[Length(CurrentLine)] = '.';
            spc := true;
            cmd := ReadS;
            end;
         if not cmd then ReadS;
         EndPara;
      end;
end;

var
   a1, h1 : string;

procedure DoList;
var n, nh : integer;
begin
   ReadS;
   while not SSComp('END.') do
      begin
      if newtitle then PrintTitle;
      newtitle := false;
      if TextMode
         then ShowText
         else
            begin
            h1 := header;
            case ptyp of
               SpellsType :
                  begin
                  a1 := CurrentLine;
                  if lbl <> NoHeader then ReadS;
                  end;
               PotionType :
                  if MKLCol = 0
                     then a1 := 'LEVEL '+strv(AML)+' POTIONS'
                     else a1 := 'POTIONS';
               ChargedType :
                  if MKLCol = 0
                     then a1 := 'LEVEL '+strv(AML)+' '+ItemTypeNames[ItemType]+' CHARGED ITEMS'
                     else a1 := ItemTypeNames[ItemType]+' CHARGED ITEMS';
               PermanentType :
                  if MKLCol = 0
                     then a1 := 'LEVEL '+strv(AML)+' '+ItemTypeNames[ItemType]+' PERMANENT ITEMS'
                     else a1 := ItemTypeNames[ItemType]+' PERMANENT ITEMS';
               EnchantmentType :
                  a1 := 'ENCHANTMENTS';
               StorerType :
                  a1 := 'STORERS';
               GrowerType :
                  a1 := 'GROWERS';
               OtherType :
                  a1 := 'OTHER MAGIC ITEMS';
               end;
            n := ReadAll;
            if n > 0 then
               begin
               if lbl = InHeader
                  then insertstring(a1,h1,hpos,hlen,LeftField,width,LeftField);
               if hdr <> NoHeader
                  then
                     begin
                     GetCellText(0, h1);
                     nh := 1;
                     end
                  else nh := 0;
               ShowList(n, nh, a1, true);
               end;
            end;
      ReadS;
      end;
end;

procedure DoSpellList;
begin
   fileis := 1;
   GetInFile(2);
   GetOutFile(3);
   StartDoc;
   DoList;
   {dofooter;}
   EndDoc;
   close(fout);
   close(fin);
end;

Begin
   HelpText[HelpIndex] := 'RUN L {infile}  {outfile}                               spell list';
   inc(HelpIndex);
   CallProcs[callindex] := DoSpellList;
   CallChars[callindex] := 'L';
   inc(callindex);
End.

