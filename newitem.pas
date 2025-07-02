(* NEWITEM.PAS
*)
unit NewItem;

interface

uses Defs, LDefs, Costs;

procedure NewItems;

{----------------------------------------------------------------------------------}
implementation
const
   NumMPPot = 10;           { should be even }
   NumMPF2  = NumMPPot div 2;

type
   mpfa = array[1..NumMPPot] of real;

const
   mpf : mpfa = (1.0,1.5,2.0,3.0,4.0,5.0,6.0,8.0,10.0,12.0);

procedure Potions;
var i, n, j, mp, AML : integer;
begin
   PrintFormattedText('Potions', StyleH2, 15, 12, true);
   PrintFormattedText('Prices for SR 5 potions. 5x for Sweet '+
                      'until 8th then 2x.  D is the number of '+
                      'days to create the potion.',
                      StyleBody, 12, 12, false);
   Lbl := NoHeader;
   NumRowsLine := 3;
   CellMask := 'LL';
   for i := 1 to NumMPPot do
      CellMask := CellMask + ' RRR RRRR RR';
   Width := length(CellMask);
   FindCells;
   Cells[0,1] := 'Lvl';
   for i := 1 to NumMPPot do
      begin
      Cells[0,i*3-1] := 'MP';
      Cells[0,i*3  ] := 'Cost';
      Cells[0,i*3+1] := 'D';
      end;
   n := 0;
   for AML := 4 to MaxLevel do
      for j := 0 to 0 do
         begin
         inc(n);
         Cells[n,1] := StrV(AML);
         for i := 1 to NumMPPot do
            begin
            mp := round(mpf[j * NumMPF2 + i] * AML);
            Cells[n,i*3-1] := StrV(mp);
            Cells[n,i*3  ] := Pot(AML, mp, 4, 10, BaseSR);
            Cells[n,i*3+1] := StrR(Time, 0);
            end;
         end;
   ShowList(n, 1, '', true);

   { The following replaces the old "extended" chart: }
   PrintFormattedText('The above prices are for SR 5 potions (a spell at '+
                      'the maker''s level.  A potion that has a higher SR '+
                      'will cost somewhat less, using the following chart:',
                      StyleBody, 12, 12, false);
   CellMask := 'LLLLL';
   Cells[0, 1] := 'SR';
   Cells[1, 1] := 'Cost';
   for i := 4 to 20 do
      begin
      CellMask := CellMask + ' RRRRRR';
      Cells[0, i-2] := StrV(i);
      Cells[1, i-2] := 'x' + StrR(sr[5]/sr[i] * 100, 0) + '%';
      end;
   Width := length(CellMask);
   FindCells;
   ShowList(1, 1, '', true);
end;

procedure Charges;
var ItemType    : ItemTypes;
    i, AML, Row : integer;
   procedure Charge1(Col, MP : integer);
   begin
      Cells[Row, Col*3-1] := StrV(MP);
      Cells[Row, Col*3  ] := Charge(ItemType, AML, MP, 4, 10);
      Cells[Row, Col*3+1] := PTimeDays(Time, 5);
   end;
begin
   if MaxLevel < ChargesMinLevel[ItemTargeted] then exit;
   PrintFormattedText('Charged Items', StyleH2, 15, 12, true);
   Lbl := NoHeader;
   CellMask := 'LLLLL';
   Cells[0, 1] := 'Level';
   for i := 1 to 5 do
      begin
      CellMask := CellMask + ' RRRRR RRRRRR RRRRR';
      Cells[0, i*3-1] := 'MP';
      Cells[0, i*3  ] := 'Cost';
      Cells[0, i*3+1] := 'Time';
      end;
   Width := Length(CellMask);
   FindCells;

   for ItemType := ItemTargeted to Targeted do
      if MaxLevel >= ChargesMinLevel[ItemType]
         then
            begin
            PrintFormattedText(ItemTypeNames[ItemType]+' ITEMS', StyleH3, 12, 12, true);
            for AML := ChargesMinLevel[ItemType] to MaxLevel do
               begin
               Row := AML+1-ChargesMinLevel[ItemType];
               Cells[Row, 1] := StrV(AML);
               Charge1(1, AML+1);
               Charge1(2, AML+6);
               Charge1(3, 24);
               Charge1(4, 32);
               Charge1(5, AML*12);
               end;
            ShowList(MaxLevel + 1 - ChargesMinLevel[ItemType], 1, '', true);
            end;
end;

procedure Permanents;
var ItemType    : ItemTypes;
    i, AML, Row, RoundTo : integer;
    MPCost, SPCost, SpellCost, MPTime, SPTime, SpellTime : real;
begin
   if MaxLevel < PermMinLevel[ItemTargeted] then exit;
   PrintFormattedText('Permanent Items', StyleH2, 15, 12, true);
   PrintFormattedText('Prices and times are per Initial MP, SP, and Spell Level.', StyleNorm, 12, 12, false);
   Lbl := NoHeader;
   CellMask := 'LLLLL';
   Cells[0, 1] := 'Level';
   Cells[0, 2] := 'MP Cost';
   Cells[0, 3] := 'SP Cost';
   Cells[0, 4] := 'Spell Cost';
   Cells[0, 5] := 'MP Time';
   Cells[0, 6] := 'SP Time';
   Cells[0, 7] := 'Spell Time';
   Cells[0, 8] := ' Max Spell Level';
   for i := 2 to 7 do
      CellMask := CellMask + ' RRRRRRRRRR';
   CellMask := CellMask + ' LLLLLLLLLLLLLLL';
   Width := Length(CellMask);
   FindCells;

   for ItemType := ItemTargeted to SpellTransfer do
      if MaxLevel >= PermMinLevel[ItemType]
         then
            begin
            if ItemType = SpellTransfer
               then PrintFormattedText(ItemTypeNames[ItemType]+'S', StyleH3, 12, 12, true)
               else PrintFormattedText(ItemTypeNames[ItemType]+' ITEMS', StyleH3, 12, 12, true);
            for AML := PermMinLevel[ItemType] to MaxLevel do
               begin
               if ItemType = SpellTransfer
                  then RoundTo := 10
                  else RoundTo := 100;
               PermanentV(ItemType, AML, 1, 1, 1, MPCost, SPCost, SpellCost, MPTime, SPTime, SpellTime);
               Row := AML+1-PermMinLevel[ItemType];
               Cells[Row, 1] := StrV(AML);
               Cells[Row, 2] := ValStr(MPCost,    8, RoundTo);
               Cells[Row, 3] := ValStr(SPCost,    8, RoundTo);
               Cells[Row, 4] := ValStr(SpellCost, 8, RoundTo);
               Cells[Row, 5] := PTimeDays(MPTime,    5);
               Cells[Row, 6] := PTimeDays(SPTime,    5);
               Cells[Row, 7] := PTimeDays(SpellTime, 5);
               Cells[Row, 8] := ' ' + StrV(AML+PermMaxSpellLevel[ItemType]);
               end;
            ShowList(MaxLevel + 1 - PermMinLevel[ItemType], 1, '', true);
            end;
end;

procedure Enchantments;
var Plus, AML : integer;
   procedure wrt(s1 : string; EffL : integer; s2 : string);
   begin
      Cells[Plus+1, (AML-8)*2+2] := s1 + ValStr(Enchantment(Plus, AML, EffL, false), 4, 50);
      Cells[Plus+1, (AML-8)*2+3] := PTimeDays(Time, 4) + s2;
   end;
begin
   if MaxLevel < 8 then exit;
   PrintFormattedText('Weapon Enchantments', StyleH2, 15, 12, true);
   CellMask := 'LLLL';
   for AML := 8 to MaxLevel do
      CellMask := CellMask + ' RRRRR RRRR';
   Width := length(CellMask);
   FindCells;
   Cells[-1, 1] := '';
   for AML := 8 to MaxLevel do
      begin
      Cells[-1, (AML-8)*2+2] := 'Level';
      Cells[-1, (AML-8)*2+3] := StrV(AML);
      end;
   Cells[0, 1] := 'Plus';
   for AML := 8 to MaxLevel do
      begin
      Cells[0, (AML-8)*2+2] := 'Cost';
      Cells[0, (AML-8)*2+3] := 'Time';
      end;

   for Plus := 0 to MaxLevel - 7 do
      begin
      Cells[Plus+1, 1] := '+' + StrV(Plus);
      for AML := 8 to MaxLevel do if plus > (aml - 7)
         then
            begin
            Cells[Plus+1, (AML-8)*2+2] := '';
            Cells[Plus+1, (AML-8)*2+3] := '';
            end
         else
            if plus = (aml - 7)
               then wrt('(', +1, ')')
               else wrt('' ,  0, '');
      end;
   ShowList(MaxLevel - 6, 2, '', true);
   PrintFormattedText('Rust Protection, Break Protection, Rot Protection, '+
                      'Drop Protection, Fire Protection, and Waterproofing '+
                      'as +0.  Values in parenthesis are for Strong Power clerics',
                      StyleBody, 12, 12, false);
end;

procedure Foci;
var AML, i : integer;
   procedure Storer(I, M, MinLevel, RoundTo : integer);
   begin
      if AML < MinLevel
         then
            begin
            Cells[AML-9, I * 2 + 2] := '';
            Cells[AML-9, I * 2 + 3] := '';
            end
         else
            begin
            Cells[AML-9, I * 2 + 2] := ValStr(StorerV(AML, M), 4, RoundTo);
            Cells[AML-9, I * 2 + 3] := PTimeDays(Time, 5);
            end;
   end;
   procedure grower(I, M, RoundTo : integer);
   begin
      Cells[AML-9, I * 2 + 10] := ValStr(GrowerV(AML, M), 4, RoundTo);
      Cells[AML-9, I * 2 + 11] := PTimeDays(Time, 5);
   end;
begin
   if MaxLevel < 10 then exit;
   PrintFormattedText('MP Storage Devices', StyleH2, 15, 12, true);

   Cells[-1, 1] := '';
   Cells[-1, 2] := 'Internal Storer';
   Cells[-1, 3] := '(3/1)';
   Cells[-1, 4] := 'Internal Storer';
   Cells[-1, 5] := '(1/1)';
   Cells[-1, 6] := 'External Storer';
   Cells[-1, 7] := '(3/1)';
   Cells[-1, 8] := 'External Storer';
   Cells[-1, 9] := '(1/1)';
   Cells[0, 1] := 'Level';
   CellMask := 'LLLLL';
   for i := 0 to 3 do
      begin
      CellMask := CellMask + ' RRRRRRRR RRRRR';
      Cells[0, i*2+2] := 'Cost';
      Cells[0, i*2+3] := 'Time';
      end;
   if MaxLevel >= 14 then
      begin
      Cells[-1, 10] := 'Internal Grower';
      Cells[-1, 11] := '';
      Cells[-1, 12] := 'External Grower';
      Cells[-1, 13] := '';
      CellMask := CellMask + ' RRRRRRRR RRRRR RRRRRRRR RRRRR';
      Cells[0, 10] := 'Cost';
      Cells[0, 11] := 'Time';
      Cells[0, 12] := 'Cost';
      Cells[0, 13] := 'Time';
      end;
   Width := length(CellMask);
   FindCells;
   for AML := 10 to MaxLevel do
      begin
      Cells[AML-9, 1] := StrV(AML);
      Storer(0,  1, 10, 50);
      Storer(1,  2, 11, 100);
      Storer(2,  5, 11, 100);
      Storer(3, 10, 12, 1000);
      if AML >= 14
         then
            begin
            Grower(0,  1, 1000);
            Grower(1,  2, 1000);
            end
         else
            begin
            Cells[AML-9, 10] := '';
            Cells[AML-9, 11] := '';
            Cells[AML-9, 12] := '';
            Cells[AML-9, 13] := '';
            end;
      end;
   ShowList(MaxLevel - 9, 2, '', true);
end;

procedure UsedPrices;
var i, j, k : integer;
begin
   PrintFormattedText('Used Charged Items', StyleH2, 15, 12, true);
   PrintFormattedText('This chart indicates what the magic shop sells used '+
                      'charged items for as a percentage of their original '+
                      'price (they buy at 2/3 this price).  The '+
                      'cost is based on the original SR (down) and the '+
                      'current SR (across).', StyleBody, 12, 12, false);
   Lbl := NoHeader;
   CellMask := 'RRRR RRRR';
   for i := lowsr to msr+4
      do CellMask := CellMask + ' RRRR';
   Width := Length(CellMask);
   FindCells;
   Cells[-1,1] := 'Orig';
   Cells[ 0,1] := 'SR';
   Cells[-1,2] := '-20';
   Cells[ 0,2] := 'to -4';
   for i := lowsr to msr+4 do
      begin
      Cells[ 0,i-LowSR+3] := StrV(i);
      Cells[-1,i-LowSR+3] := '';
      end;
   k := 0;
   for j := 5 to msr do
      begin
      inc(k);
      Cells[k, 1] := StrV(j);
      Cells[k, 2] := '6';
      for i := lowsr to msr+4
         do if pv[j,i] > 0.06
            then Cells[k,i-LowSR+3] := StrR(pv[j,i]*100,0)
            else Cells[k,i-LowSR+3] := '6'
      end;
   inc(k);
   Cells[k,1] := 'Uses>';
   Cells[k,2] := '-4';
   for i := lowsr to msr+4
      do Cells[k,i-LowSR+3] := StrR(pr[i],1);
   inc(k);
   Cells[k,1] := 'Uses>';
   Cells[k,2] := '-21';
   for i := lowsr to msr+4
      do Cells[k,i-LowSR+3] := StrR(pr2[i],1);
   ShowList(k, 2, '', true);
end;

procedure Wages;
var i : integer;
begin
   CellMask := 'LLLLLLLLLLLLL';
   for i := 1 to MaxLevel do
      CellMask := CellMask + ' RRRR';
   Width := length(CellMask);
   FindCells;
   Lbl := StyleH2;
   Columns := 1;
   Cells[0, 1] := 'Level';
   for i := 1 to MaxLevel
      do Cells[0, i+1] := StrV(i);
   Cells[1, 1] := 'Time (sp/day)';
   for i := 1 to MaxLevel
      do Cells[1, i+1] := StrR(mumake[i],0);
   Cells[2, 1] := 'MP/day';
   for i := 1 to MaxLevel
      do Cells[2, i+1] := StrV(mppday[i]);
   Cells[3, 1] := 'sp/MP';
   for i := 1 to MaxLevel
      do Cells[3, i+1] := StrR(muchg[i],2);
   Cells[4,1] := 'sp/SP';
   for i := 1 to MaxLevel
      do Cells[4, i+1] := StrR(clchg[i],2);
   ShowList(4, 1, 'MAGIC USER and CLERIC WAGES', true);
end;

procedure NewItems;
begin
   GetOutFile(2);
   GetParameterInt(MaxLevel,3);
   GetParameterInt(msr,4);
   GetParameterInt(SPDivisor,5);
   StartDoc;
   Potions;
   Charges;
   Wages;
   UsedPrices;
   Enchantments;
   Permanents;
   Foci;
   EndDoc;
   close(fout);
end;

Begin
   HelpText[HelpIndex] := 'RUN M {outfile} {max lev} {max SR} {SP div}             for new magic items';
   inc(HelpIndex);
   HelpText[HelpIndex] := '                {def=14}  {def=15} {def=21}';
   inc(HelpIndex);
   CallProcs[callindex] := newitems;
   CallChars[callindex] := 'M';
   inc(callindex);
End.
