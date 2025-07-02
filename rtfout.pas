(* RTFOUT.PAS
*)
unit RTFOut;

interface

uses Defs;

const
   twips_per_inch  = 1440;
   twips_per_point = 20;
   number_of_tabs  = 100;

type
   FontFamilies = (FamNil, FamRoman, FamSwiss, FamModern, FamScript,  FamDecor, FamTech, FamBidi);
   FontPitches = (PitchDefault, PitchFixed, PitchVariable);
   Font = class
      Next   : Font;
      Family : FontFamilies;
      Pitch  : FontPitches;
      Name   : string;
      Handle : string;
      constructor Create(FFamily : FontFamilies;
                         FPitch  : FontPitches;
                         FName   : string;
                         FHandle : string;
                         FNext   : Font);
      procedure show(fnum : integer);
      end;
   FontTable = class
      FontList : Font;
      constructor Create(Font_List : Font);
      procedure show;
      end;
   RTFDocument = class
      Fonts : FontTable;
      constructor Create(RFonts : FontTable);
      procedure show;
      procedure finish;
      end;
   PageFormat = class
      Width         : longint;
      Height        : longint;
      LeftMargin    : longint;
      RightMargin   : longint;
      TopMargin     : longint;
      BottomMargin  : longint;
      Gutter        : longint;
      MirrorMargins : boolean;
      constructor Create;
      procedure show;
      end;
   SectionBreaks = (SbrkNone, SbrkColumn, SbrkPage, SbrkEven, SbrkOdd);
   SectionFormat = class
      Break         : SectionBreaks;
      Columns       : integer;
      ColumnSpace   : longint;
      constructor Create;
      procedure show;
      end;
   ParagraphAlignments = (PalgnLeft, PalgnCenter, PalgnRight, PalgnJustify);
   TabTypes = (TabLeft, TabRight, TabCenter, TabDecimal, TabBar);
   TabLeaders = (TabLdrNone, TabLdrDot, TabLdrHyphen, TabLdrUline, TabLdrThick, TabLdrEqual);
   TabRange = 1..number_of_tabs;
   BorderTypes = (BdrSingle, BdrThick, BdrDouble, BdrShadow, BdrDot, BdrHair);
   UnderLineTypes = (UlineNone, UlineContinuous, UlineWord, UlineDotted,
                     UlineDashed, UlineDashDotted, UlineDashDotDotted,
                     UlineDouble, UlineHeavyWave, UlineLongDashed, UlineThick,
                     UlineThickDotted, UlineThickDashed, UlineThickDashDotted,
                     UlineThickDashDotDotted, UlineThickLongDashed,
                     UlineDoubleWave, UlineWave);
   ParagraphFormat = class
      Alignment         : ParagraphAlignments;
      FirstIndent       : longint;
      LeftIndent        : longint;
      RightIndent       : longint;
      SpaceBefore       : longint;
      SpaceAfter        : longint;
      LineSpacing       : longint;
      InTable           : boolean;
      Keep              : boolean;
      KeepNext          : boolean;
      PageBreak         : boolean;
      WidowControl      : boolean;
      NumberTabs        : integer;
      Tabs              : array [TabRange] of longint;
      TabType           : array [TabRange] of TabTypes;
      TabLeader         : array [TabRange] of TabLeaders;
      BorderTop         : boolean;
      BorderTypeTop     : BorderTypes;
      BorderWidthTop    : longint;
      BorderSpaceTop    : longint;
      BorderBottom      : boolean;
      BorderTypeBottom  : BorderTypes;
      BorderWidthBottom : longint;
      BorderSpaceBottom : longint;
      BorderLeft        : boolean;
      BorderTypeLeft    : BorderTypes;
      BorderWidthLeft   : longint;
      BorderSpaceLeft   : longint;
      BorderRight       : boolean;
      BorderTypeRight   : BorderTypes;
      BorderWidthRight  : longint;
      BorderSpaceRight  : longint;
      Font              : integer;
      Bold              : boolean;
      Italic            : boolean;
      FontSize          : longint;
      UnderLineType     : UnderLineTypes;
      constructor Create;
      procedure show;
      end;

var
   TheFontTable : FontTable;

procedure RTFProlog;

procedure RTFPostlog;

function FindFont(var Fonts : FontTable;
                  Name      : string) : integer;

procedure Tab;

procedure EndPara;

{----------------------------------------------------------------------------------}
implementation

procedure Tab;
begin
   write(fout, '\tab ');
end;

procedure EndPara;
begin
   writeln(fout, '\par');
end;

{--------------------------}
{ PageFormat               }
{--------------------------}

constructor PageFormat.Create;
begin
   Width         := 12240;
   Height        := 15840;
   LeftMargin    := 720;
   RightMargin   := 720;
   TopMargin     := 720;
   BottomMargin  := 864;
   Gutter        := 720;
   MirrorMargins := false;
end;

procedure PageFormat.show;
begin
   write(fout, '\defformat\widowctrl\ftnbj\aenddoc');
   write(fout, '\paperw', Width);
   write(fout, '\paperh', Height);
   write(fout, '\margl',  LeftMargin);
   write(fout, '\margr',  RightMargin);
   write(fout, '\margt',  TopMargin);
   write(fout, '\margb',  BottomMargin);
   write(fout, '\gutter', Gutter);
   if MirrorMargins
      then write(fout, '\margmirror');
end;

{--------------------------}
{ SectionFormat            }
{--------------------------}

constructor SectionFormat.Create;
begin
   Break       := SbrkPage;
   Columns     := 1;
   ColumnSpace := 720;
end;

procedure SectionFormat.show;
begin
   write(fout, '\sectd');
   case Break of
      SbrkNone   : write(fout, '\sbknone');
      SbrkColumn : write(fout, '\sbkcol');
      SbrkPage   : write(fout, '\sbkpage');
      SbrkEven   : write(fout, '\sbkeven');
      SbrkOdd    : write(fout, '\sbkodd');
      end;
   if Columns > 1
   then
      begin
         write(fout, '\cols', Columns);
         write(fout, '\colsx', ColumnSpace);
      end;
end;

{--------------------------}
{ WriteBorder              }
{--------------------------}

procedure WriteBorder(var fout   : text;
                      BorderKey  : string;
                      BorderType : BorderTypes;
                      Width      : longint;
                      Space      : longint);
begin
   write(fout, BorderKey);
   case BorderType of
      BdrSingle  : write(fout, '\brdrs');
      BdrThick   : write(fout, '\brdrth');
      BdrShadow  : write(fout, '\brdrsh');
      BdrDouble  : write(fout, '\brdrdb');
      BdrDot     : write(fout, '\brdrdot');
      BdrHair    : write(fout, '\brdrhair');
      end;
   if Width <> 0
      then write(fout, '\brdrw', Width);
   if Space <> 0
      then write(fout, '\brdrsp', Space);
end;

{--------------------------}
{ ParagraphFormat          }
{--------------------------}

constructor ParagraphFormat.Create;
var i : integer;
begin
   Alignment         := PalgnLeft;
   FirstIndent       := 0;
   LeftIndent        := 0;
   RightIndent       := 0;
   SpaceBefore       := 0;
   SpaceAfter        := 0;
   LineSpacing       := 0;
   InTable           := false;
   Keep              := false;
   KeepNext          := false;
   PageBreak         := false;
   WidowControl      := false;
   NumberTabs        := 0;
   for i := 1 to number_of_tabs do
      begin
      Tabs[i]      := 0;
      TabType[i]   := TabLeft;
      TabLeader[i] := TabLdrNone;
      end;

   BorderTop         := false;
   BorderTypeTop     := BdrSingle;
   BorderWidthTop    := 0;
   BorderSpaceTop    := 0;
   BorderBottom      := false;
   BorderTypeBottom  := BdrSingle;
   BorderWidthBottom := 0;
   BorderSpaceBottom := 0;
   BorderLeft        := false;
   BorderTypeLeft    := BdrSingle;
   BorderWidthLeft   := 0;
   BorderSpaceLeft   := 0;
   BorderRight       := false;
   BorderTypeRight   := BdrSingle;
   BorderWidthRight  := 0;
   BorderSpaceRight  := 0;
   Font              := 0;
   Bold              := false;
   Italic            := false;
   FontSize          := 20;
   UnderLineType     := UlineNone;
end;

procedure ParagraphFormat.show;
var i : integer;
begin
   writeln(fout);
   write(fout, '\pard');
   case Alignment of
      PalgnLeft:      write(fout, '\ql');
      PalgnCenter:    write(fout, '\qc');
      PalgnRight:     write(fout, '\qr');
      PalgnJustify:   write(fout, '\qj');
      end;
   write(fout, '\fi', FirstIndent);
   write(fout, '\li', LeftIndent);
   write(fout, '\ri', RightIndent);
   write(fout, '\sb', SpaceBefore);
   write(fout, '\sa', SpaceAfter);
   write(fout, '\sl', LineSpacing);
   if InTable
      then write(fout, '\intbl');
   if Keep
      then write(fout, '\keep');
   if KeepNext
      then write(fout, '\keepn');
   if PageBreak
      then write(fout, '\pagebb');
   if WidowControl
      then write(fout, '\widctlpar')
      else write(fout, '\nowidctlpar');
   for i := 1 to NumberTabs do
      begin
      case TabType[i] of
         TabLeft     : ;
         TabRight    : write(fout, '\tqr');
         TabCenter   : write(fout, '\tqc');
         TabDecimal  : write(fout, '\tqdec');
         TabBar      : write(fout, '\tb');
         end;
      case TabLeader[i] of
         TabLdrNone   : ;
         TabLdrDot    : write(fout, '\tldot');
         TabLdrHyphen : write(fout, '\tlhyph');
         TabLdrUline  : write(fout, '\tlul');
         TabLdrThick  : write(fout, '\tlth');
         TabLdrEqual  : write(fout, '\tleq');
         end;
      write(fout, '\tx', Tabs[i]);
      end;
   if BorderTop
      then WriteBorder(fout, '\brdrt', BorderTypeTop, BorderWidthTop, BorderSpaceTop);
   if BorderBottom
      then WriteBorder(fout, '\brdrb', BorderTypeBottom, BorderWidthBottom, BorderSpaceBottom);
   if BorderLeft
      then WriteBorder(fout, '\brdrl', BorderTypeLeft, BorderWidthLeft, BorderSpaceLeft);
   if BorderRight
      then WriteBorder(fout, '\brdrr', BorderTypeRight, BorderWidthRight, BorderSpaceRight);
   write(fout, '\plain');
   write(fout, '\f', Font);
   write(fout, '\fs', FontSize);
   if Bold
      then write(fout, '\b')
      else write(fout, '\b0');
   if Italic
      then write(fout, '\i')
      else write(fout, '\i0');
   case UnderLineType of
      UlineNone:               write(fout, '\ul0');
      UlineContinuous:         write(fout, '\ul');
      UlineWord:               write(fout, '\ulw');
      UlineDotted:             write(fout, '\uld');
      UlineDashed:             write(fout, '\uldash');
      UlineDashDotted:         write(fout, '\uldashd');
      UlineDashDotDotted:      write(fout, '\uldashdd');
      UlineDouble:             write(fout, '\uldb');
      UlineHeavyWave:          write(fout, '\ulhwave');
      UlineLongDashed:         write(fout, '\ulldash');
      UlineThick:              write(fout, '\ulth');
      UlineThickDotted:        write(fout, '\ulthd');
      UlineThickDashed:        write(fout, '\ulthdash');
      UlineThickDashDotted:    write(fout, '\ulthdashd');
      UlineThickDashDotDotted: write(fout, '\ulthdashdd');
      UlineThickLongDashed:    write(fout, '\ulthldash');
      UlineDoubleWave:         write(fout, '\uldbwave');
      UlineWave:               write(fout, '\ulwave');
      end;
   writeln(fout);
end;

{--------------------------}
{ RTFProlog                }
{--------------------------}

procedure RTFProlog;
begin
   write(fout, '{\rtf1\ansi\deff0{\fonttbl{\f0 Times New Roman;}}');
end;

{--------------------------}
{ RTFPostlog               }
{--------------------------}

procedure RTFPostlog;
begin
   write(fout, '}');
end;

{--------------------------}
{ RTFDocument              }
{--------------------------}

constructor RTFDocument.Create(RFonts : FontTable);
begin
   Fonts := RFonts;
end;

procedure RTFDocument.show;
begin
   write(fout, '{\rtf1\ansi\deff0');
   Fonts.show;
   writeln(fout);
end;

procedure RTFDocument.finish;
begin
   writeln(fout, '}');
end;

{--------------------------}
{ FontTable                }
{--------------------------}

constructor FontTable.Create(Font_List : Font);
begin
   FontList := Font_List;
end;

procedure FontTable.show;
var aFont : Font;
    i    : integer;
begin
   aFont := FontList;
   i     := 0;
   writeln(fout, '{\fonttbl');
   while aFont <> nil do
      begin
         aFont.show(i);
         inc(i);
         aFont := aFont.Next;
      end;
   writeln(fout, '}');
end;

{--------------------------}
{ Font                     }
{--------------------------}

constructor Font.Create(FFamily : FontFamilies;
                      FPitch  : FontPitches;
                      FName   : string;
                      FHandle : string;
                      FNext   : Font);
begin
   Next   := FNext;
   Family := FFamily;
   Pitch  := FPitch;
   Name   := FName;
   Handle := FHandle;
end;

procedure Font.show(fnum : integer);
begin
   write(fout, '{\f',fnum);
   case Family of
      FamNil     : write(fout, '\fnil');
      FamRoman   : write(fout, '\froman');
      FamSwiss   : write(fout, '\fswiss');
      FamModern  : write(fout, '\fmodern');
      FamScript  : write(fout, '\fscript');
      FamDecor   : write(fout, '\fdecor');
      FamTech    : write(fout, '\ftech');
      FamBidi    : write(fout, '\fbidi');
      end;
   write(fout, '\fcharset0');
   case Pitch of
      PitchDefault  : write(fout, '\fprq0');
      PitchFixed    : write(fout, '\fprq1');
      PitchVariable : write(fout, '\fprq2');
      end;
   writeln(fout, Name, '\;}');
end;

{--------------------------}
{ FindFont                 }
{--------------------------}
function FindFont(var Fonts : FontTable;
                  Name      : string) : integer;
var i : integer;
    f : Font;
begin
   i := 0;
   f := Fonts.FontList;
   while (f <> nil) and (f.Handle <> Name)
      do f := f.Next;
   if (f = nil)
      then i := 0;
   FindFont := i;
end;

Begin
   TheFontTable := FontTable.Create(
                   Font.Create(FamRoman,
                               PitchVariable,
                               'Times New Roman',
                               'TIMES',
                   Font.Create(FamSwiss,
                               PitchVariable,
                               'Arial',
                               'ARIAL',
                   Font.Create(FamModern,
                               PitchFixed,
                               'Courier New',
                               'COURIER',
                   nil))));
End.
