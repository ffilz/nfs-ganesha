(* LDEFS.PAS
*)

unit LDefs;

interface

uses Defs, RTFOut;

const
   MaxNumI = 100;
   LeftField = 1;
   RightField = 2;
   CenterField = 3;
   StyleH1     = 101;
   StyleH2     = 102;
   StyleH3     = 103;
   StyleH4     = 104;
   StyleH5     = 105;
   StyleBody   = 106;
   StyleNorm   = 107;
   StringEqual = 0;
   StringGreater = 1;
   StringLess = 2;
   StringGreatEq = 3;
   StringLessEq = 4;
   StringNotEqual = 5;
   ValueEqual = 10;
   ValueGreater = 11;
   ValueLess = 12;
   ValueGreatEq = 13;
   ValueLessEq = 14;
   ValueNotEqual = 15;
   SortBefore = -1;
   SortSame = 0;
   SortAfter = 1;
   NoHeader = 255;
   InHeader = 254;
   SpellsType = 0;
   PotionType = 1;
   ChargedType = 2;
   PermanentType = 3;
   StorerType = 4;
   GrowerType = 5;
   EnchantmentType = 6;
   OtherType = 7;
   MaxFooterLines = 12;
   lbl : integer = CenterField;
   hdr : integer = NoHeader; {NotInHeader;}
   NumFooterLines : integer = 0; { number of footer lines }
   hpos : integer = 1;
   hlen : integer = 132;
   width : integer = 132;
   columns : integer = 1;
   titlepos : integer = CenterField;
   header : string = '';
   title : string = '';
   identstring : string = '';
   ptyp : integer = SpellsType;
   MaxCols = 20;
   newtitle : boolean = false;
   pconc : boolean = false;
   textmode : boolean = false;
   QuickSortType = 1;
   ShellSortType = 2;
   BubbleSortType = 3;
   SortType : integer = 1;
   NumSorts : integer = 0;
   {NumSorts1 : integer = 0;}
   TwipsPerChar : integer = 80;
   NumCells    : integer = 0;
   NumCellsRow : integer = 0;
   NumRowsLine : integer = 0;
   Style       : integer = StyleBody;
   CellMask : string = '';
   PositionMask : string = '';

   { Cold Iron RTF Template }
   DoH1        = '\s1\qc\sa360\keep\keepn\pagebb\widctlpar\brdrt\brdrtnthsg\brdrw45\brsp20 \brdrb\brdrthtnsg\brdrw45\brsp20 \adjustright \b\f4\fs32\kerning28\cgrid ';
   DoH2        = '\s2\sb120\sa240\keep\keepn\widctlpar\brdrt\brdrs\brdrw45\brsp20 \adjustright \b\f4\fs28\cgrid ';
   DoH3        = '\s3\sb120\sa240\keepn\widctlpar\adjustright \b\f4\ul\cgrid ';
   DoH4        = '\s4\sa240\keepn\widctlpar\adjustright \b\f4\cgrid ';
   DoH5        = '\s5\sa60\keepn\widctlpar\adjustright \i\f4\fs20\cgrid ';
   DoNorm      = '\widctlpar\adjustright \f4\fs20\cgrid ';
   DoBody      = '\s15\sa240\widctlpar\adjustright \f4\fs20\cgrid ';
   DoBlock     = '\s17\li1440\ri1440\sa120\widctlpar\adjustright \f4\fs20\cgrid ';
   DoFont      = '\f4';
   PivotRow    = -11;

type
   longintarray = array[0..maxnumi] of longint;

var
   Cells : array[PivotRow..1000, 1..MaxNumI] of string;
   footer : array[0..MaxFooterLines-1] of string;       { footer lines              }
   footerpos : array[0..MaxFooterLines-1] of integer;   { centering of footer lines }
   SortCols, SortDir, SRs : longintarray;
   fileis : integer;
   CellX       : array[1..100] of longint;
   CellA       : array[1..100] of integer;
   Positions   : array[0..100] of byte;
   Ident       : array[1..100] of boolean;
   ARow        : array[1..100] of string;


{ Get a time in days from row and column of cells, number is appended with
  d, w, m, or y }
function GetTimeDays(Row, Col : integer; var error : integer) : real;

{ get an integer from row and column of cells }
function GetInt(Row, Col : integer; var Error : integer) : longint;

{ extract an integer from string s, starting at position pos for length len }
function ExtractInt(s : string; pos, len : integer; var error : integer) : longint;

{ extract a number of integers from string s, starting at position pos for length len,
  the integers are captured into the array ints, and the number found is returned in numi }
procedure ExtractInts(s : string; pos, len : integer; var ints : longintarray; var numi : integer);

{ center, left adjust, or right adjust string into field of len characters }
function makefield(s1 : string; len, typ : integer) : string;

{ use typ2 to center, left, or right adjust s2 into field of len2 characters,
  then use typ1 to center, left, or right adjust s1 into field of len1 characters,
  then copy adjusted s1 into position pos of s2 }
procedure insertstring(s1 : string; var s2 : string;
                       pos, len1, typ1, len2, typ2 : integer);

{ Start a Cold Iron template document }
procedure StartDoc;

{ End a Cold Iron template document }
procedure EndDoc;

{ Start a Cold Iron template paragraph }
procedure StartPara(Fmt, Size, SpaceAfter : integer; Bold : boolean);

{ End a Cold Iron template paragraph }
procedure EndPara;

procedure PrintFormattedText(Text : string; Fmt, Size, SpaceAfter : integer; Bold : boolean);

procedure PrintTitle;

procedure ShowList(Rows, NumHeaderRows : integer;
                   TheLabel            : string;
                   BlankAfter          : boolean);

{ Sort rows functions }
procedure BubbleSortRows(n : integer);
procedure ShellSortRows(n : integer);
procedure QuickSortRows(i, j : integer);

procedure BlankRow(Row : integer);

function FinishCells(n : integer) : integer;

procedure FindCells;

procedure FindPositions;

procedure GetCellText(Entry : integer; s :  string);

{----------------------------------------------------------------------------------}
implementation

var
   xstring : string;

function GetTimeDays(Row, Col : integer; var error : integer) : real;
var c : char;
    r : real;
    i  : longint;
    j  : integer;
    s1, XString : string;
begin
   j := 1;
   XString := Cells[Row, Col];
   c := s1[length(XString)];
   s1 := '';
   c  := ' ';
   while (j <= length(XString)) and (c = ' ') do
      begin
      if XString[j] in ['0'..'9','-','+']
         then s1 := s1 + XString[j]
         else c := XString[j];
      inc(j);
      end;
   if s1 = '' then s1 := 'error';
   val(s1,r,error);
   if error <> 0
      then r := 0.0;
   case c of
      ' ','d' : ;
      'w'     : r := r * 7.0;
      'm'     : r := r * 30.0;
      'y'     : r := r * 365.0;
      else      r := 0.0;
                error := 1;
      end;
   GetTimeDays := r;
end;

function GetInt(Row, Col : integer; var error : integer) : longint;
var i  : longint;
    j  : integer;
    s1 : string;
    clear : boolean;
begin
   j := 1;
   XString := Cells[Row, Col];
   s1 := '';
   clear := false;
   while (j <= length(Cells[Row, Col])) do
      begin
      if XString[j] in ['0'..'9','-','+']
         then s1 := s1 + XString[j]
         else if XString[j] = '*' then Clear := true;
      inc(j);
      end;
   if s1 = '' then s1 := 'error';
   val(s1,i,error);
   if error <> 0
      then i := 0;
   GetInt := i;
   if Clear then Cells[Row, Col] := '';
end;

function ExtractInt(s : string; pos, len : integer; var error : integer) : longint;
var i : longint;
    j : integer;
    s1 : string;
begin
   j := pos;
   s1 := '';
   while (j < (pos + len)) and (j <= length(s)) do begin
      if s[j] in ['0'..'9','-','+'] then s1 := s1 + s[j];
      inc(j);
      end;
   if s1 = '' then s1 := 'error';
   val(s1,i,error);
   if error <> 0 then begin
      xstring := copy(s,pos,len);
      i := 0;
      end;
   ExtractInt := i;
end;

procedure ExtractInts(s : string; pos, len : integer; var ints : longintarray; var numi : integer);
var
   i : longint;
   err, j, ej : integer;
   s1 : string;
begin
   numi := 0;
   j := pos;
   if (pos + len - 1) < length(s)
      then ej := pos + len - 1
      else ej := length(s);
   repeat
      s1 := '';
      if s[j] in ['-','+'] then begin
         s1 := s[j];
         inc(j);
         end;
      while (j <= ej) and (s[j] in ['0'..'9']) do
         begin
         s1 := s1 + s[j];
         inc(j);
         end;
      val(s1,i,err);
      if (err = 0) and (numi < maxnumi) then
         begin
         inc(numi);
         ints[numi] := i;
         end;
      { skip non-numeric characters }
      while (j <= ej) and not (s[j] in ['-','+','0'..'9']) do inc(j);
   until j > ej;
end;

function makefield(s1 : string; len, typ : integer) : string;
var s2 : string;
    ulen : integer;
begin
   ulen := len - length(s1);
   fillchar(s2,256,' ');
   SetLength(s2, 255);
   case typ of
       leftfield : s2 := s1;
      rightfield : s2 := copy(s2,1,ulen) + s1;
     centerfield : s2 := copy(s2,1,ulen div 2) + s1 +
                         copy(s2,1,(ulen + 1) div 2);
     end;
   SetLength(s2, len);
   makefield := s2;
end;

{ use typ2 to center, left, or right adjust s2 into field of len2 characters,
  then use typ1 to center, left, or right adjust s1 into field of len1 characters,
  then copy adjusted s1 into position pos of s2 }
procedure insertstring(s1 : string; var s2 : string;
                       pos, len1, typ1, len2, typ2 : integer);
var i : integer;
    s : string;
begin
   s2 := makefield(s2,len2,typ2);
   s := makefield(s1,len1,typ1);
   for i := 0 to len1 - 1 do s2[pos+i] := s[i+1];
end;

procedure StartDoc;
begin
   writeln(fout, '{\rtf1\ansi\ansicpg1252\uc1 \deff0\deflang1033\deflangfe1033');
   writeln(fout, '{\fonttbl');
   writeln(fout, '{\f0\froman\fcharset0\fprq2{\*\panose 02020603050405020304}Times New Roman;}');
   writeln(fout, '{\f4\froman\fcharset0\fprq2{\*\panose 00000000000000000000}Times;}}');
   writeln(fout, '{\colortbl;\red0\green0\blue0;');
   writeln(fout, '\red0\green0\blue255;\red0\green255\blue255;\red0\green255\blue0;\red255\green0\blue255;\red255\green0\blue0;\red255\green255\blue0;\red255\green255\blue255;\red0\green0\blue128;\red0\green128\blue128;\red0\green128\blue0;\red128\green0\blue128;');
   writeln(fout, '\red128\green0\blue0;\red128\green128\blue0;\red128\green128\blue128;\red192\green192\blue192;}');
   writeln(fout, '{\stylesheet');
   writeln(fout, '{', DoNorm, '\snext0 Normal;}');
   writeln(fout, '{', DoH1, '\sbasedon0 \snext15 heading 1;}');
   writeln(fout, '{', DoH2, '\sbasedon0 \snext15 heading 2;}');
   writeln(fout, '{', DoH3, '\sbasedon0 \snext15 heading 3;}');
   writeln(fout, '{', DoH4, '\sbasedon0 \snext15 heading 4;}');
   writeln(fout, '{', DoH5, '\sbasedon0 \snext15 heading 5;}');
   writeln(fout, '{\*\cs10 \additive Default Paragraph Font;}');
   writeln(fout, '{', DoBody, '\sbasedon0 \snext15 Body Text;}');
   writeln(fout, '{', DoBlock, '\sbasedon0 \snext17 Block Text;}}');
   writeln(fout, '\margl1440\margr720\margt720\margb864 \widowctrl\ftnbj\aenddoc\margmirror\noextrasprl\prcolbl\cvmme');
   writeln(fout, '\sprsspbf\brkfrm\swpbdr\hyphcaps0\fracwidth\viewkind4\viewscale100\pgbrdrhead\pgbrdrfoot \fet0{\*\template ');
   writeln(fout, 'D:\\msoffice\\winword\\Templates\\My Templates\\coldiron.dot}\sectd \linex0\endnhere\pgbrdropt32');
   writeln(fout, '\sectdefaultcl {\*\pnseclvl1\pnucrm\pnstart1\pnindent720\pnhang{\pntxta .}}{\*\pnseclvl2\pnucltr\pnstart1');
   writeln(fout, '\pnindent720\pnhang{\pntxta .}}{\*\pnseclvl3');
   writeln(fout, '\pndec\pnstart1\pnindent720\pnhang{\pntxta .}}{\*\pnseclvl4\pnlcltr\pnstart1\pnindent720\pnhang{\pntxta )}}');
   writeln(fout, '{\*\pnseclvl5\pndec\pnstart1\pnindent720\pnhang{\pntxtb (}{\pntxta )}}');
   writeln(fout, '{\*\pnseclvl6\pnlcltr\pnstart1\pnindent720\pnhang{\pntxtb (}{\pntxta )}}');
   writeln(fout, '{\*\pnseclvl7\pnlcrm\pnstart1\pnindent720\pnhang{\pntxtb (}{\pntxta )}}');
   writeln(fout, '{\*\pnseclvl8\pnlcltr\pnstart1\pnindent720\pnhang{\pntxtb (}{\pntxta )}}');
   writeln(fout, '{\*\pnseclvl9\pnlcrm\pnstart1\pnindent720\pnhang{\pntxtb (}{\pntxta )}}');
end;

procedure EndDoc;
begin
   writeln(fout, '}');
end;

procedure StartPara(Fmt, Size, SpaceAfter : integer; Bold : boolean);
begin
   write(fout, '\pard \plain ');
   if Fmt in [CenterField, LeftField, RightField]
      then
      begin
      write(fout, '\sa', SpaceAfter * 2, DoFont, '\fs', Size*2);
      if Bold then write(fout, '\b ');
      end;
   case Fmt of
      CenterField: write(fout, '\qc ');
      LeftField:   write(fout, '\ql ');
      RightField:  write(fout, '\qr ');
      StyleH1:     write(fout, DoH1);
      StyleH2:     write(fout, DoH2);
      StyleH3:     write(fout, DoH3);
      StyleH4:     write(fout, DoH4);
      StyleH5:     write(fout, DoH5);
      StyleNorm:   write(fout, DoNorm);
      StyleBody:   write(fout, DoBody);
      end;
   writeln(fout);
end;

procedure EndPara;
begin
   writeln(fout, '\par ');
end;

procedure PrintFormattedText(Text : string; Fmt, Size, SpaceAfter : integer; Bold : boolean);
begin
   StartPara(Fmt, Size, SpaceAfter, Bold);
   write(fout, Text);
   EndPara;
end;

procedure PrintTitle;
begin
   PrintFormattedText(Title, TitlePos, 20, 20, true);
end;

procedure StyleARow(IsHeader, HasBorder, BoldBorder :  boolean);
var i : integer;
begin
   write(fout, '\trowd ');
   if IsHeader then write(fout, '\trhdr');
   writeln(fout);
   for i := 1 to NumCellsRow do
      begin
      write(fout, '\clvertalt ');
      if HasBorder then
         if BoldBorder
            then write(fout, '\clbrdrb\brdrs\brdrw30')
            else write(fout, '\clbrdrb\brdrs\brdrw10');
      write(fout, '\cltxlrtb \cellx', CellX[i]);
      writeln(fout);
      end;
end;

procedure ShowARow(IsHeader, IsLast : boolean);
var i : integer;
begin
   write(fout, '\pard \plain ', DoFont, '\fs16\widctlpar\adjustright \intbl\keep\keepn\cgrid {');
   if IsHeader
      then write(fout, '\b ')
      else write(fout, '\b0');
   for i := 1 to NumCellsRow do
      begin
      case CellA[i] of
         LeftField:      write(fout, '\ql ');
         CenterField:    write(fout, '\qc ');
         RightField:     write(fout, '\qr ');
         end;
      if (i > 1) and (CellA[i-1] = RightField) and (CellA[i] = LeftField)
         then write(fout, ' ');
      write(fout, ARow[i], '\cell ');
      end;
   (*if IsLast
      then writeln(fout, '}\lastrow ')
      else *)
   writeln(fout, '}\row ');
end;

procedure ShowList(Rows, NumHeaderRows : integer;
                   TheLabel            : string;
                   BlankAfter          : boolean);
var c, i, j, r : integer;
begin
   if not (lbl in [NoHeader, InHeader])
      then PrintFormattedText(TheLabel, lbl, 15, 12, true);
   for i := 1 - NumHeaderRows to 0 do
      begin
      for c := 0 to Columns-1 do
         begin
         for j := 1 to NumCells do
            ARow[j + c * (NumCells + 1)] := Cells[i, j];
         if c < Columns then ARow[(c + 1) * (NumCells + 1)] := '';
         end;
      StyleARow(true, i = 0, i = 0);
      ShowARow(true, false);
      end;
   StyleARow(false, false, false);
   for i := 1 to Rows do
      begin
      if (NumRowsLine <> 0) then
         if ((i mod NumRowsLine) = 0) or (i = Rows)
            then StyleARow(false, true, false)
            else if (i > 1) and ((i mod NumRowsLine) = 1)
               then StyleARow(false, false, false);
      for c := 0 to Columns-1 do
         begin
         r := i + c * Rows;
         for j := 1 to NumCells do
            ARow[j + c * (NumCells + 1)] := Cells[r, j];
         if c < Columns then ARow[(c + 1) * (NumCells + 1)] := '';
         end;
      ShowARow(false, i = Rows);
      end;
   if BlankAfter
      then PrintFormattedText('', StyleNorm, 20, 0, false);
end;

{ compare Cells[r1, Column] against Cells[r2, Column] using comparison of type ctyp,
  which can be string or numeric compare,
  result is SortBefore, SortSame, SortEqual }
function SortColumns(r1, r2 : integer; Column, CType : byte) : integer;
var e                : integer;
    i1, i2           : longint;
    SCBefore, SCSame : boolean;
begin
   if CType in [ValueGreater, ValueLess] then
      begin
      i1 := ExtractInt(Cells[r1, Column], 1, Length(Cells[r1, Column]), e);
      i2 := ExtractInt(Cells[r2, Column], 1, Length(Cells[r2, Column]), e);
      if CType = ValueLess
         then SCBefore := i1 > i2
         else SCBefore := i1 < i2;
      SCSame := i1 = i2;
      end
   else
      begin
      if CType = StringLess
         then SCBefore := Cells[r1, Column] > Cells[r2, Column]
         else SCBefore := Cells[r1, Column] < Cells[r2, Column];
      SCSame := Cells[r1, Column] = Cells[r2, Column];
      end;
   if SCSame
      then SortColumns := SortSame
      else if SCBefore
         then SortColumns := SortBefore
         else SortColumns := SortAfter;
end;

function QSComp(a, b : integer) : integer;
var i, r : integer;
begin
   QSComp := SortSame;
   for i := 1 to NumSorts do
      begin
      r := SortColumns(a, b, SortCols[i], SortDir[i]);
      if r <> SortSame
         then
            begin
            QSComp := r;
            exit;
            end;
      end;
end;

procedure CopyPivotRow(r : integer);
var i : integer;
begin
   for i := 1 to MaxNumI do
      Cells[PivotRow, i] := Cells[r, i];
end;

procedure SwapRows(r1, r2 : integer);
var i : integer;
    t : string;
begin
   for i := 1 to MaxNumI do
      begin
      t            := Cells[r1, i];
      Cells[r1, i] := Cells[r2, i];
      Cells[r2, i] := t;
      end;
end;

procedure BubbleSortRows(n : integer);
var i, j : integer;
begin
   for i := 1 to n-1 do
      for j := i+1 to n do
         if QSComp(i, j) = SortAfter
            then SwapRows(i, j);
end;

procedure ShellSortRows(n : integer);
var i, j, gap : integer;
begin
   gap := n div 2;
   while (gap > 0) do begin
      for i := gap to n-1 do begin
         j := i - gap;
         while (j >= 0) and (QSComp(j+1, j+gap+1) = SortAfter) do begin
            SwapRows(j+1, j+gap+1);
            dec(j,gap);
            end;
         end;
      gap := gap div 2;
      end;
end;

const qsloops : integer = 0;

procedure QuickSortRows(i, j : integer);
var L, R, pivot : integer;
   procedure FindPivot;
   var p : integer;
   begin
      pivot := 0;
      for p := i+1 to j do
         begin
         if QSComp(p, i) = SortAfter
            then pivot := p
            else if QSComp(i, p) = SortAfter
               then pivot := i;
         if pivot <> 0 then exit;
         end;
   end;
begin
   FindPivot;
   inc(qsloops);
   if pivot <> 0 then begin
      CopyPivotRow(pivot);
      L := i;
      R := j;
      repeat
         SwapRows(L, R);
         while QSComp(PivotRow, L) =  SortAfter do inc(L);
         while QSComp(PivotRow, R) <> SortAfter do dec(R);
      until L > R;
      if (L-1) > i then QuickSortRows(i, L-1);
      if (j > L) then QuickSortRows(L, j);
      end;
end;

procedure BlankRow(Row : integer);
var i : integer;
begin
   for i := 1 to MaxNumI do Cells[Row, i] := '';
end;

function FinishCells(n : integer) : integer;
var Rows, j, k : integer;
begin
   case sorttype of
      QuickSortType  : QuickSortRows(1, n);
      ShellSortType  : ShellSortRows(n);
      BubbleSortType : BubbleSortRows(n);
      end;
   { blank entries to fill last column }
   for j := n+1 to n+columns do BlankRow(j);

   { clear out text when the line above will have identical text }
   rows := (n + columns - 1) div columns;
   for j := n downto 2 do if ((j - 1) mod rows) <> 0 then
      for k := 1 to NumCells do
         if Ident[k] and (Cells[j-1, k] = Cells[j, k]) then
            Cells[j, k] := IdentString;
   FinishCells := Rows;
end;

procedure FindCells;
var i, j : integer;
    Pos  : longint;
begin
   CellMask := CellMask + ' L';
   NumCells := 0;
   i  := 1;
   Positions[0] := 1;
   while i <= Width do
      begin
      inc(NumCells);
      { record the paragraph alignment of the cell }
      case UpCase(CellMask[i]) of
         'C' : CellA[NumCells] := CenterField;
         'R' : CellA[NumCells] := RightField;
         else  CellA[NumCells] := LeftField;
         end;
      Ident[NumCells] := (CellMask[i] >= 'a') and (CellMask[i] <= 'z');
      { skip character positions }
      while (i <= length(CellMask)) and (CellMask[i] <> ' ') do inc(i);
      { now we have a cell mark }
      while (i < length(CellMask)) and (CellMask[i] = ' ') do inc(i); { skip cell mark }
      CellX[NumCells] := i * TwipsPerChar;
      Positions[NumCells] := i;
      end;
   Pos := Width+1;
   NumCellsRow := NumCells;
   for j := 2 to Columns do
      begin
      { handle space between super-columns as a cell }
      inc(Pos, 2);
      inc(NumCellsRow);
      CellX[NumCellsRow] := Pos * TwipsPerChar;
      CellA[NumCellsRow] := LeftField;
      { handle each column in next super-column }
      for i := 1 to NumCells do
         begin
         inc(NumCellsRow);
         Positions[NumCellsRow] := Positions[i];
         CellX[NumCellsRow]     := (Positions[i] + Pos) * TwipsPerChar;
         CellA[NumCellsRow]     := CellA[i];
         end;
      { skip the width of this super-column }
      inc(Pos, Positions[NumCells]);
      end;
end;

procedure FindPositions;
var i, n : integer;
begin
   n := 0;
   i := 1;
   Positions[0] := 1;
   while i <= length(PositionMask) do
      begin
      inc(n);
      { skip character positions }
      while (i <= length(PositionMask)) and (PositionMask[i] <> ' ') do inc(i);
      { now we have a cell mark }
      while (i < length(PositionMask)) and (PositionMask[i] = ' ') do inc(i); { skip cell mark }
      Positions[n] := i;
      end;
end;

procedure GetCellText(Entry : integer; s :  string);
var i, e, f : integer;
begin
   if NumCells = 0 then
      begin
      fillchar(CellMask,256,'L');
      SetLength(CellMask, Width);
      FindCells;
      end;
   for i := 1 to NumCells do
      begin
      f := Positions[i-1];
      e := Positions[i] - 1;
      while (f <= e) and (s[f] = ' ') do inc(f);
      while (e >= f) and (s[e] = ' ') do dec(e);
      if e >= f
         then Cells[Entry, i] := Copy(s, f, e-f+1)
         else Cells[Entry, i] := '';
      end;
end;

Begin
   footer[0] := '--------';
   footerpos[0] := CenterField;
End.
