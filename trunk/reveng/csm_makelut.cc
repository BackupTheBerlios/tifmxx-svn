char
CSM::MakeLUT()
{
	char rc = 0;
	short wPhyBlock = 0; // r15
	short wOldPhyBlock = 0;
	short wLogBlock = 0;  // rsp + 0x34, ebp
	char lvar_x30 = 1;   // rsp + 0x30
	char byZone = 0;     // sil
	short wLastPageLogBlock = 0;

	do { // 2bc41 <- 2c65a
		if (byZone >= mbyZones || wPhyBlock >= mwPhyBlocks) {
			var_x147 = 1;
			break;
		}

		if (lvar_x30) { // 2bc5e -> 2bce8
			for (wLogBlock = 0; wLogBlock < mwLogBlocks; wLogBlock++) {
				if (byZone < mbyZones && wLogBlock < mwLogBlocks)
					var_x148[byZone * mwLogBlocks + wLogBlock] = -1;
			}
			wPhyBlock = 0;
			mpFirstEmptyPhyBlock[byZone] = -1;
		} // 2bce8

		if (!byZone || wPhyBlock == var_x108) {
			if (byZone < mbyZones && wPhyBlock < mwPhyBlocks)
				var_x150[wPhyBlock >> 3] |= 1 << (wPhyBlock & 7);
		} else { //2bceb, 2bcf5 -> 2c4d1
			write32(base_addr + 0xa0, (mwPhyBlocks * byZone + wPhyBlock) * mbyPhyBlockSize);
			KeSynchronizeExecution(card_int, ClearStatus, &dwSM_STATUS);
			write32(base_addr + 0x94, 0x109);
			rc = sub1fee0(&sm_event_x0d0, -1000000);
			if (!rc && vara_4)
				rc = 0x86;
			if (!rc && KeSynchronizeExecution(card_int, &CSM::CheckTimeOut, &dwSM_STATUS))
				rc = 0x6a;
			if (!rc && KeSynchronizeExecution(card_int, &CSM::CheckDataErr, &dwSM_STATUS))
				rc = 0x62;
			if (rc) {
				if (0x8000 & read32(base_addr + 0x98))
					var_x158++;
				rc = 0;
			}

			if (0x17ff != (0x17ff & read32(base_addr + 0xb4))) { // 2be3f
				if (byZone < mbyZones && wPhyBlock < mwPhyBlocks)
					var_x150[(mwPhyBlocks >> 3) * byZone + (wPhyBlock >> 3)] |= 1 << (wPhyBlock & 7);

				if (!(0x8000 & read32(base_addr + 0x98))) { // 2bea0
					if (!sub28cb0(&wLogBlock) && wLogBlock != -1) { // 2bedc
						if (byZone < mbyZones && wLogBlock < mwLogBlocks && var_x148[mwLogBlocks * byZone + wLogBlock] != -1) {
							rc = sub28b80(byZone, wPhyBlock, mbyLogBlockSize - 1);
							if (rc) {
								if (rc == 0x86)
									return rc;
								wOldPhyBlock = (byZone < mbyZones && wLogBlock < mwLogBlocks) ? var_x148[mwLogBlocks * byZone + wLogBlock] : -1;
								rc = sub28b80(byZone, wOldPhyBlock, mbyLogBlockSize - 1);
								if (rc) {
									if (rc == 0x86)
										return rc;
								} else { // 2c00a
									if (EraseBlock(byZone, wPhyBlock))
										return 0x6d;
								}
							} else { // 2c03d
								wLastPageLogBlock = 0x3ff & (read32(base_addr + 0xb4) >> 1);
								if (wLastPageLogBlock == wLogBlock) {
									wOldPhyBlock = (byZone < mbyZones && wLogBlock < mwLogBlocks) ? var_x148[mwLogBlocks * byZone + wLogBlock] : -1;
									rc = sub28b80(byZone, wOldPhyBlock, mbyLogBlockSize - 1);
									if (rc) {
										if (rc == 0x86)
											return rc;
										if (byZone < mbyZones && wLogBlock < mwLogBlocks)
											var_x148[mwLogBlocks * byZone + wLogBlock] = wPhyBlock;
										if (EraseBlock(byZone, wOldPhyBlock))
											return 0x6d;
									} else { // 2c162
										wLastPageLogBlock = 0x3ff & (read32(base_addr + 0xb4) >> 1);
										if (wLastPageLogBlock == wLogBlock) {
											if (EraseBlock(byZone, wPhyBlock))
												return 0x6d;
										} else { // 2c1fc
											if (byZone < mbyZones && wLogBlock < mwLogBlocks)
												var_x148[mwLogBlocks * byZone + wLogBlock] = wPhyBlock;
											if (EraseBlock(byZone, wOldPhyBlock))
												return 0x6d;
										}
									}
								} else { // 2c266
									wOldPhyBlock = (byZone < mbyZones && wLogBlock < mwLogBlocks) ? var_x148[mwLogBlocks * byZone + wLogBlock] : -1;
									rc = sub28b80(byZone, wOldPhyBlock, mbyLogBlockSize - 1);
									if (rc) {
										if (rc == 0x86)
											return rc;
									} else { // 2c301
										if (EraseBlock(byZone, wPhyBlock))
											return 0x6d;
									}
								}
							}
						} else { // 2c336
							if (byZone < mbyZones && wLogBlock < mwLogBlocks)
								var_x148[mwLogBlocks * byZone + wLogBlock] = wPhyBlock;
						}
					}
				} else { // 2c391
					// print stuff
				}
			} else { // 2c3aa
				int cnt = 0;
				int t_val = read32(base_addr + 0xb4) >> 0x10;
				while (t_val) {
					cnt += t_val & 1;
					t_val >>= 1;
				}

				if (cnt >= 7) {
					if (mpFirstEmptyPhyBlock[byZone] == -1)
						mpFirstEmptyPhyBlock[byZone] = wPhyBlock;

					if (byZone < mbyZones && wPhyBlock < mwPhyBlocks)
						var_x150[(mwPhyBlocks >> 3) * byZone + (wPhyBlock >> 3)] &= ~(1 << (wPhyBlock & 7));
				} else { // 2c473
					if (byZone < mbyZones && wPhyBlock < mwPhyBlocks)
						var_x150[(mwPhyBlocks >> 3) * byZone + (wPhyBlock >> 3)] |= 1 << (wPhyBlock & 7);
				}
			}
		} // 2c4d1
		wPhyBlock++;
		if (wPhyBlock != mwPhyBlocks)
			continue; // -> 2c64e

		var_x100[byZone] = mpFirstEmptyPhyBlock[byZone];
		for (wLogBlock = 0; wLogBlock < mwLogBlocks; wLogBlock++) {
			if (byZone >= mbyZones || wLogBlock >= mwLogBlocks || var_x148[mwLogBlocks * byZone + wLogBlock] == -1) {
				if (byZone < mbyZones && wLogBlock < mwLogBlocks)
					var_x148[mwLogBlocks * byZone + wLogBlock) = var_x100[mwLogBlocks * byZone + wLogBlock];
			}
		} // 2c5c2

		if (byZone < mbyZones && var_x100[byZone] < mwPhyBlocks)
			var_x150[(mwPhyBlocks >> 3) * byZone + (var_x100[byZone] >> 3)] |= 1 << (var_x100[byZone] & 7);

		byZone++;
		lvar_x30 = 1;
		wPhyBlock = 0;
		if (byZone == mbyZones) {
			var_x147 = lvar_x30;
			wPhyBlock = sub27950(0, 0);
		}
	} while (!var_x147); // 2c65a -> 2bc41
	return 0;
}
