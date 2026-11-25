// Places that form an address in the owned-car arrays. Each line becomes a
// midasm hook run right after the instruction at `addr` (hooks.toml, the same
// table) and a function OwnedCars_<addr> in owned_cars.cpp.
//   OWNED_CARS_SITE(CAR, addr, reg)     reg holds a LocalOptions-based address (or
//                                       the slot's product about to be added to it)
//                                       into the 8176-byte car slots: += car delta
//   OWNED_CARS_SITE(DAMAGE, addr, reg)  same for the 24-byte damage slots: += damage delta
//   OWNED_CARS_SITE(PLATES, addr, reg)  reg holds owner+137972, the plate bytes
//                                       themselves: becomes the moved plate array
//   OWNED_CARS_SITE_OWNER(PLATES, addr, reg, owner_reg)
//                                       reg is about to be read with +137972 and holds
//                                       owner+slot, the owner in owner_reg: moved by
//                                       plates - (owner+137972)

OWNED_CARS_SITE(CAR, 8226DA54, r3)  // sub_8226D8F8: copies the player's vehicle config into the current slot (sub_8238EE58)
OWNED_CARS_SITE(CAR, 82276200, r31)  // sub_822761D0: reads the config of slot a3 into the player vehicle (sub_8238F648); r31 kept for a null check
OWNED_CARS_SITE(CAR, 8228192C, r3)  // sub_822811C8: reads the class of the current car (sub_8238F6B0)
OWNED_CARS_SITE(CAR, 822821B0, r29)  // sub_82282148: reads the current car (sub_8238F998, sub_8238F1E8); r29 lives to the end
OWNED_CARS_SITE(CAR, 822855B0, r4)  // sub_822854E8: lobby label: passes the slot config to sub_82285398 (class + name +8141, no capture)
OWNED_CARS_SITE(CAR, 8228831C, r3)  // sub_82288040: class filter on a slot (sub_823906D8)
OWNED_CARS_SITE(CAR, 8228A0D8, r3)  // sub_82289F20: ranked: class of the current car (sub_8238F6B0)
OWNED_CARS_SITE(CAR, 82290F1C, r3)  // sub_822901D8: class of the current car (sub_8238F6B0)
OWNED_CARS_SITE(CAR, 822AABC4, r4)  // sub_822AAA18: streams slot obj[5] into the vehicle object (sub_8238EFE0)
OWNED_CARS_SITE(CAR, 822CF838, r3)  // sub_822CF6E8: reads the current car (sub_8238F998)
OWNED_CARS_SITE(CAR, 8238AA68, r30)  // sub_8238AA30: remove slot: start of the shift loop (copy slot i+1 -> i, step +8176)
OWNED_CARS_SITE(CAR, 8238AAB0, r30)  // sub_8238AA30: remove slot: resets the last slot (count*8176 - 6816)
OWNED_CARS_SITE(CAR, 8238AC04, r30)  // sub_8238ABB8: sell: start of the shift loop (copy slot i+1 -> i, step +8176)
OWNED_CARS_SITE(CAR, 8238AC3C, r30)  // sub_8238ABB8: sell: resets slot [count]
OWNED_CARS_SITE(CAR, 8238AF10, r3)  // sub_8238AEC0: validity check of the current car (sub_8238FE90)
OWNED_CARS_SITE(CAR, 8238B600, r25)  // sub_8238B5E0: naming: target slot (sub_8238F1E8, sub_8238F968)
OWNED_CARS_SITE(CAR, 8238B720, r29)  // sub_8238B6E8: add car: slot base after add (copy to +1360, stw +9460)
OWNED_CARS_SITE(CAR, 8238C670, r29)  // sub_8238C5A0: post-load reconcile: slot base after add (r28=+9469, stw +9448, +1360; also feeds 8238C880/8238C938)
OWNED_CARS_SITE(CAR, 8238CB58, r11)  // sub_8238CB28: script native: returns the slot config pointer through the message
OWNED_CARS_SITE(CAR, 8238CBA4, r3)  // sub_8238CB78: script native: copies a config into a slot (sub_8238EE58)
OWNED_CARS_SITE(CAR, 8269E020, r3)  // sub_8269DFE0: select car: reads the slot config (sub_8269A1E0)
OWNED_CARS_SITE(CAR, 826A4744, r30)  // sub_826A4718: garage: slot config (name +8109, sub_8269A1E0, sub_8238F998)
OWNED_CARS_SITE(CAR, 826A724C, r3)  // sub_826A7170: garage list: writes the current car config (sub_8238EE58)
OWNED_CARS_SITE(CAR, 826A7DFC, r24)  // sub_826A7D58: garage details: slot config (sub_8238F1F0, name +8109, sub_8238F6B0)
OWNED_CARS_SITE(CAR, 826A9918, r3)  // sub_826A98E0: sell: vehicle id of the listed slot (sub_8238F1E8)
OWNED_CARS_SITE(CAR, 826A9AC0, r3)  // sub_826A9A88: trade-in: vehicle id of the current car (sub_8238F1E8)
OWNED_CARS_SITE(CAR, 826A9DB0, r3)  // sub_826A9D58: copies a config into the current slot (sub_8238EE58)
OWNED_CARS_SITE(CAR, 826AA45C, r3)  // sub_826AA3C0: copies a config into the current slot (sub_8238EE58)
OWNED_CARS_SITE(CAR, 826AA9A4, r3)  // sub_826AA930: copies a config into the current slot (sub_8238EE58)
OWNED_CARS_SITE(CAR, 826AE7E8, r3)  // sub_826AB520: garage state machine: vehicle id of a slot (sub_8238F1E8)
OWNED_CARS_SITE(CAR, 826AFF60, r8)  // sub_826AFF20: reads byte slot+8108 (lbz 9468(r8)); base after add
OWNED_CARS_SITE(CAR, 826B2098, r30)  // sub_826B2068: RMR: slot config passed to sub_8238FD38 and sub_826B1C88 (copied, not captured)
OWNED_CARS_SITE(CAR, 826B2CBC, r4)  // sub_826B2800: copies the current car config out (sub_8238F648)
OWNED_CARS_SITE(CAR, 826B9E2C, r3)  // sub_826B9DF8: class of the current car (sub_8238F6B0)
OWNED_CARS_SITE(CAR, 826BA1C8, r3)  // sub_826BA198: class of the current car (sub_8238F6B0)
OWNED_CARS_SITE(CAR, 82275ED4, r11)  // sub_82275E60: plate cache build loop: slot base (offset counter + LocalOptions), reads cfg+2119
OWNED_CARS_SITE(CAR, 82285C8C, r3)  // sub_82285C08: counts slots that pass a class filter (sub_823906D8); offset counter loop
OWNED_CARS_SITE(CAR, 8228B0C0, r30)  // sub_8228B018: car list UI: slot config (name +8141, sub_823906D8, sub_8238F6B0); offset counter loop
OWNED_CARS_SITE(CAR, 8269A174, r3)  // sub_8269A140: counts slots with the same vehicle id; offset counter loop
OWNED_CARS_SITE(CAR, 826A736C, r30)  // sub_826A7170: garage list loop: slot config (r29=+8141 derived after)
OWNED_CARS_SITE(CAR, 8238AB74, r31)  // sub_8238AB50: find slot by name (+8109), step +8176
OWNED_CARS_SITE(CAR, 8238AF40, r30)  // sub_8238AEC0: find first valid slot (sub_8238FE90), step +8176
OWNED_CARS_SITE(CAR, 8238B440, r29)  // sub_8238B3F8: pre-save loop: writes the vehicle id into cfg+8088 of every slot
OWNED_CARS_SITE(CAR, 8238B4D0, r11)  // sub_8238B4A0: find slot by hash (+8104), step +8176
OWNED_CARS_SITE(CAR, 8238B624, r29)  // sub_8238B5E0: naming: counts duplicates of a vehicle id, step +8176
OWNED_CARS_SITE(CAR, 8238BC74, r29)  // sub_8238BA38: debug add car: finds slot by name (+8109), step +8176
OWNED_CARS_SITE(CAR, 8238BD04, r3)  // sub_8238BA38: debug add car: copies the new config into slot 0 (sub_8238EFE0)
OWNED_CARS_SITE(CAR, 8238C2C4, r28)  // sub_8238C278: new game: default cars written per roster entry (gated <30), step +8176
OWNED_CARS_SITE(CAR, 8238C898, r31)  // sub_8238C5A0: post-load reconcile: find slot by name (+8109), step +8176
OWNED_CARS_SITE(DAMAGE, 8238ACF4, r11)  // sub_8238ACD8: damage writer: slot*24 + LocalOptions, base of 5 stfsx (+253108..+253124)
OWNED_CARS_SITE(DAMAGE, 8238AD34, r8)  // sub_8238ACD8: damage writer: (slot+10547)*24, 6th stfsx with LocalOptions in r3 (+253128)
OWNED_CARS_SITE(DAMAGE, 8238B790, r7)  // sub_8238B6E8: add car: zeroes the 6 damage floats of the new slot
OWNED_CARS_SITE(DAMAGE, 826A7FBC, r8)  // sub_826A7D58: garage details: reads damage[slot] for cur_damage
OWNED_CARS_SITE(PLATES, 82275ECC, r29)  // sub_82275E60: plate cache writer: owner+137972, stbx per slot without a cap
OWNED_CARS_SITE_OWNER(PLATES, 82276218, r10, r29)  // sub_822761D0: plate cache reader: owner(r29)+slot, lbzx with 137972
OWNED_CARS_SITE_OWNER(PLATES, 826B2CD8, r7, r11)  // sub_826B2800: plate cache reader: owner(r11)+current slot, lbzx with 137972
