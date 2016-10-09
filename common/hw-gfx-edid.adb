--
-- Copyright (C) 2016 secunet Security Networks AG
--
-- This program is free software; you can redistribute it and/or modify
-- it under the terms of the GNU General Public License as published by
-- the Free Software Foundation; version 2 of the License.
--
-- This program is distributed in the hope that it will be useful,
-- but WITHOUT ANY WARRANTY; without even the implied warranty of
-- MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
-- GNU General Public License for more details.
--

with HW;

with HW.Debug;
with GNAT.Source_Info;

use type HW.Byte;
use type HW.Pos16;
use type HW.Word16;

package body HW.GFX.EDID is

   function Checksum_Valid (Raw_EDID : Raw_EDID_Data) return Boolean
   with
      Pre => True
   is
      Sum : Byte := 16#00#;
   begin
      for I in Raw_EDID_Index loop
         Sum := Sum + Raw_EDID (I);
      end loop;
      pragma Debug (Sum /= 16#00#, Debug.Put_Line
        (GNAT.Source_Info.Enclosing_Entity & ": EDID checksum invalid!"));
      return Sum = 16#00#;
   end Checksum_Valid;

   function Valid (Raw_EDID : Raw_EDID_Data) return Boolean
   is
      Header_Valid : Boolean;
   begin
      Header_Valid :=
         Raw_EDID (0) = 16#00# and
         Raw_EDID (1) = 16#ff# and
         Raw_EDID (2) = 16#ff# and
         Raw_EDID (3) = 16#ff# and
         Raw_EDID (4) = 16#ff# and
         Raw_EDID (5) = 16#ff# and
         Raw_EDID (6) = 16#ff# and
         Raw_EDID (7) = 16#00#;
      pragma Debug (not Header_Valid, Debug.Put_Line
        (GNAT.Source_Info.Enclosing_Entity & ": EDID header pattern mismatch!"));

      return Header_Valid and then Checksum_Valid (Raw_EDID);
   end Valid;

   ----------------------------------------------------------------------------

   REVISION                            : constant :=         19;
   INPUT                               : constant :=         20;
   INPUT_DIGITAL                       : constant := 1 * 2 ** 7;
   INPUT_DIGITAL_DEPTH_SHIFT           : constant :=          4;
   INPUT_DIGITAL_DEPTH_MASK            : constant := 7 * 2 ** 4;
   INPUT_DIGITAL_DEPTH_UNDEF           : constant := 0 * 2 ** 4;
   INPUT_DIGITAL_DEPTH_RESERVED        : constant := 7 * 2 ** 4;

   ----------------------------------------------------------------------------

   function Read_LE16
     (Raw_EDID : Raw_EDID_Data;
      Offset   : Raw_EDID_Index)
      return Word16
   is
   begin
      return Shift_Left (Word16 (Raw_EDID (Offset + 1)), 8) or
             Word16 (Raw_EDID (Offset));
   end Read_LE16;

   function Has_Preferred_Mode (Raw_EDID : Raw_EDID_Data) return Boolean
   is
   begin
      return
         Int64 (Read_LE16 (Raw_EDID, DESCRIPTOR_1)) * 10_000
            in Frequency_Type and
         ( Raw_EDID (DESCRIPTOR_1 +  2) /= 0 or
          (Raw_EDID (DESCRIPTOR_1 +  4) and 16#f0#) /= 0) and
         ( Raw_EDID (DESCRIPTOR_1 +  8) /= 0 or
          (Raw_EDID (DESCRIPTOR_1 + 11) and 16#c0#) /= 0) and
         ( Raw_EDID (DESCRIPTOR_1 +  9) /= 0 or
          (Raw_EDID (DESCRIPTOR_1 + 11) and 16#30#) /= 0) and
         ( Raw_EDID (DESCRIPTOR_1 +  3) /= 0 or
          (Raw_EDID (DESCRIPTOR_1 +  4) and 16#0f#) /= 0) and
         ( Raw_EDID (DESCRIPTOR_1 +  5) /= 0 or
          (Raw_EDID (DESCRIPTOR_1 +  7) and 16#f0#) /= 0) and
         ((Raw_EDID (DESCRIPTOR_1 + 10) and 16#f0#) /= 0 or
          (Raw_EDID (DESCRIPTOR_1 + 11) and 16#0c#) /= 0) and
         ((Raw_EDID (DESCRIPTOR_1 + 10) and 16#0f#) /= 0 or
          (Raw_EDID (DESCRIPTOR_1 + 11) and 16#03#) /= 0) and
         ( Raw_EDID (DESCRIPTOR_1 +  6) /= 0 or
          (Raw_EDID (DESCRIPTOR_1 +  7) and 16#0f#) /= 0);
   end Has_Preferred_Mode;

   function Preferred_Mode (Raw_EDID : Raw_EDID_Data) return Mode_Type
   is
      Base : constant := DESCRIPTOR_1;
      Mode : Mode_Type;

      function Read_12
        (Lower_8, Upper_4  : Raw_EDID_Index;
         Shift             : Natural)
         return Word16
      is
      begin
         return
            Word16 (Raw_EDID (Lower_8)) or
            (Shift_Left (Word16 (Raw_EDID (Upper_4)), Shift) and 16#0f00#);
      end Read_12;

      function Read_10
        (Lower_8, Upper_2  : Raw_EDID_Index;
         Shift             : Natural)
         return Word16
      is
      begin
         return
            Word16 (Raw_EDID (Lower_8)) or
            (Shift_Left (Word16 (Raw_EDID (Upper_2)), Shift) and 16#0300#);
      end Read_10;

      function Read_6
        (Lower_4     : Raw_EDID_Index;
         Lower_Shift : Natural;
         Upper_2     : Raw_EDID_Index;
         Upper_Shift : Natural)
         return Word8
      is
      begin
         return
            (Shift_Right (Word8 (Raw_EDID (Lower_4)), Lower_Shift) and 16#0f#)
            or
            (Shift_Left (Word8 (Raw_EDID (Upper_2)), Upper_Shift) and 16#30#);
      end Read_6;
   begin
      Mode := Mode_Type'
        (Dotclock             => Pos64 (Read_LE16 (Raw_EDID, Base)) * 10_000,
         H_Visible            => Pos16 (Read_12 (Base +  2, Base +  4, 4)),
         H_Sync_Begin         => Pos16 (Read_10 (Base +  8, Base + 11, 2)),
         H_Sync_End           => Pos16 (Read_10 (Base +  9, Base + 11, 4)),
         H_Total              => Pos16 (Read_12 (Base +  3, Base +  4, 8)),
         V_Visible            => Pos16 (Read_12 (Base +  5, Base +  7, 4)),
         V_Sync_Begin         => Pos16 (Read_6  (Base + 10, 4, Base + 11, 2)),
         V_Sync_End           => Pos16 (Read_6  (Base + 10, 0, Base + 11, 4)),
         V_Total              => Pos16 (Read_12 (Base +  6, Base +  7, 8)),
         H_Sync_Active_High   => (Raw_EDID (Base + 17) and 16#02#) /= 0,
         V_Sync_Active_High   => (Raw_EDID (Base + 17) and 16#04#) /= 0,
         BPC                  =>
           (if Raw_EDID (REVISION) < 4 or
               (Raw_EDID (INPUT) and INPUT_DIGITAL) = 16#00# or
               (Raw_EDID (INPUT) and INPUT_DIGITAL_DEPTH_MASK) = INPUT_DIGITAL_DEPTH_UNDEF or
               (Raw_EDID (INPUT) and INPUT_DIGITAL_DEPTH_MASK) = INPUT_DIGITAL_DEPTH_RESERVED
            then
               Auto_BPC
            else
               4 + 2 * Pos64 (Shift_Right
                 (Raw_EDID (INPUT) and INPUT_DIGITAL_DEPTH_MASK,
                  INPUT_DIGITAL_DEPTH_SHIFT))));

      -- Calculate absolute values from EDID relative values.
      Mode.H_Sync_Begin := Mode.H_Visible    + Mode.H_Sync_Begin;
      Mode.H_Sync_End   := Mode.H_Sync_Begin + Mode.H_Sync_End;
      Mode.H_Total      := Mode.H_Visible    + Mode.H_Total;
      Mode.V_Sync_Begin := Mode.V_Visible    + Mode.V_Sync_Begin;
      Mode.V_Sync_End   := Mode.V_Sync_Begin + Mode.V_Sync_End;
      Mode.V_Total      := Mode.V_Visible    + Mode.V_Total;

      return Mode;
   end Preferred_Mode;

end HW.GFX.EDID;
