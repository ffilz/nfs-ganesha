(* DEFS.PAS
*)

Unit Defs;

interface

var
   fin, fout : text;                        { input and output files }
   HelpText  : array[0..31] of string;      { big array of strings }
   CallChars : array[0..31] of char;        { program function characters }
   CallProcs : array[0..31] of procedure;   { program function routines }
   HelpIndex : integer;
   CallIndex : integer;                     { number of CallChars/Procs entries used }

{ Get integer parameter from paramstr(p) }
procedure GetParameterInt(var i : integer; p : integer);

{ get real number parameter from paramstr(p) }
procedure GetParmeterReal(var r : real; p : integer);

{ get input file from paramstr(p) }
procedure GetInFile(p : integer);

{ get output file from paramstr(p) }
procedure GetOutFile(p : integer);

{ write file size }
procedure WrtSize;

{ convert an integer to a string }
function StrV(i : longint) : string;

{ convert a real number to a string }
function StrR(r : real; dec : integer) : string;

{ convert a real number to a string, rounding any fraction up }
function StrRUp(r : real) : string;

procedure Help;

{----------------------------------------------------------------------------------}
implementation

var
   InFileName, OutFileName : string;        { input and output file names }
   Save1 : pointer;

function StrV(i : longint) : string;
var s : string;
begin
   str(i,s);
   StrV := s;
end;

function StrR(r : real; dec : integer) : string;
var s : string;
begin
   str(r:1:dec,s);
   StrR := s;
end;

function StrRUp(r : real) : string;
var s  : string;
    rv : longint;
    r1 : real;
begin
   r1 := r;
   if frac(r1) > 0
      then r1 := r1 + 1.0;
   rv := trunc(r1);
   str(rv,s);
   StrRUp := s;
end;

procedure GetParameterInt(var i : integer; p : integer);
var i1, e : integer;
begin
   if paramcount >= p then begin
      val(paramstr(p),i1,e);
      if e = 0 then i := i1;
      end;
end;

procedure GetParmeterReal(var r : real; p : integer);
var r1 : real;
    e : integer;
begin
   if paramcount >= p then begin
      val(paramstr(p),r1,e);
      if e = 0 then r := r1;
      end;
end;

procedure UCase(var s : string);
var i : byte;
begin
   for i := 1 to length(s) do s[i] := UpCase(s[i]);
end;

procedure GetOutFile(p : integer);
begin
   if ParamCount < p
      then OutFileName := 'RUN'
      else OutFileName := paramstr(p);
   UCase(outfilename);
   if pos('.',OutFileName) = 0
      then OutFileName := OutFileName + '.RTF';
   assign(fout,OutFileName);
   writeln('Output File: ',OutFileName);
   rewrite(fout);
end;

procedure WrtSize;
var f : file;
begin
   if pos('.',OutFileName) <> 0
      then
      begin
      assign(f,OutFileName);
      reset(f);
      writeln(filesize(f)/8:0:2,'k bytes');
      close(f);
      end;
end;

procedure GetInFile(p : integer);
begin
   if paramcount < p
      then begin
         write('An input file must be specified: ');
         readln(infilename);
         end
      else infilename := paramstr(p);
   ucase(infilename);
   if infilename[length(infilename)] = ':'
     then SetLength(infilename, Length(infilename)-1);
   if (pos('.',infilename) = 0)
      then infilename := infilename + '.IN';
   writeln('Input File:  ',infilename);
   assign(fin,infilename);
   reset(fin);
end;

procedure Help;
var i : integer;
begin
   for i := 0 to HelpIndex-1 do writeln(HelpText[i]);
   exitproc := Save1;
end;

procedure gone;
begin
   WrtSize;
   exitproc := Save1;
end;

Begin
   HelpIndex := 0;
   callindex := 0;
   save1 := exitproc;
   exitproc := @gone;
End.

