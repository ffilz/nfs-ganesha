(* SPU.PAS

   program to compute SP tables and print them to paramstr(1)
*)
unit SPU;

interface

uses Defs, RTFOut, Costs;

procedure sp;

{----------------------------------------------------------------------------------}
implementation

procedure writev(v : real; roundit : boolean);
begin
   if (v >= 100.0) or roundit
      then write(fout,round(v):1)
      else write(fout,round(v*10)/10:2:1);
end;

var
   Document  : RTFDocument;
   PageSetup : PageFormat;
   Heading   : ParagraphFormat;
   TopRow    : ParagraphFormat;
   Row       : ParagraphFormat;

procedure SetTabs(var P : ParagraphFormat);
var i, t : integer;
begin
   t := 0;
   for i := MinInt to MaxInt do
      begin
         inc(t);
         P.Tabs[t] := t * 180 * 3 + 180;
         P.TabType[t] := TabRight;
      end;
   P.NumberTabs := t;
end;

procedure GenerateTable(f : spFunc; head : string; round : boolean);
var int, level : integer;
begin
   Heading.show;
   write(fout, head);
   EndPara;
   TopRow.show;
   write(fout,'LV');
   for int := MinInt to MaxInt do
      begin
      Tab;
      write(fout,int);
      end;
   EndPara;
   Row.show;
   for level := 1 to MaxLevel do
      begin
      write(fout,level);
      for int := MinInt to MaxInt do
         begin
         Tab;
         writev(f(level, int), round);
         end;
      EndPara;
      end;
   EndPara;
end;

procedure sp;
begin
   PageSetup := PageFormat.create;
   PageSetup.Gutter := 360;
   Document  := RTFDocument.create(TheFontTable);
   Heading   := ParagraphFormat.create;
   TopRow    := ParagraphFormat.create;
   Row       := ParagraphFormat.create;
   Heading.Font     := FindFont(TheFontTable, 'TIMES');
   Heading.Bold     := true;
   Heading.FontSize := 40;
   TopRow.Font             := FindFont(TheFontTable, 'TIMES');
   TopRow.Bold             := true;
   TopRow.BorderBottom     := true;
   TopRow.BorderTypeBottom := BdrSingle;
   TopRow.FontSize         := 30;
   Row.Font     := FindFont(TheFontTable, 'TIMES');
   GetOutFile(2);
   GetParameterInt(MaxLevel,3);
   GetParameterInt(MaxInt,4);
   GetParameterInt(MinInt,5);
   GetParameterInt(SPDivisor,6);

   SetTabs(TopRow);
   SetTabs(Row);

   Document.show;
   PageSetup.show;

   GenerateTable(MageMemorization, 'MAGIC USER MEMORIZATION (INTELLIGENCE)', false);
   GenerateTable(ClericMemorization, 'CLERIC MEMORIZATION (WISDOM)', false);
   GenerateTable(SpellPoints, 'CLERIC SPELL POINTS (WISDOM)', true);

   Document.finish;
   close(fout);
end;

Begin
   HelpText[HelpIndex] := 'RUN S {outfile} {max lev} {max int} {min int} {SP div}  SP tables';
   inc(HelpIndex);
   HelpText[HelpIndex] := '                {def=14}  {def=26}  {def=8}   {def=21}';
   inc(HelpIndex);
   CallProcs[callindex] := sp;
   CallChars[callindex] := 'S';
   inc(callindex)
End.
