program run;

uses
  Costs in 'costs.pas',
  Defs in 'defs.pas',
  LDefs in 'ldefs.pas',
  SpList in 'splist.pas',
  NewItem in 'newitem.pas',
  SPU in 'spu.pas',
  stats in 'stats.pas',
  stats_old in 'stats_old.pas',
  rtfout in 'rtfout.pas';

procedure runit(c : char);
var i : integer;
begin
   for i := 0 to callindex-1 do
      if c = CallChars[i]
         then begin
            CallProcs[i];
            exit;
            end;
   help;
end;

var
   s : string;

begin
   if paramcount > 0
      then s := paramstr(1)
      else s := '?';
   runit(upcase(s[1]));
end.
