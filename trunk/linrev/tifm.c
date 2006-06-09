// use 2.6.11.6
B40:
int
do_tifm_transfer(struct request_queue *q)
{
	struct request_queue *var_60 = q;
B50:
	struct request *var_30 = elv_next_request(var_60);
	int var_44 = 0;
	if(!var_30) goto F07;
	if(!(var_30->flags & 0x10)) goto EE4; // not fs rw
	var_2c = var_30->rq_disk->private_data;

	var_35 = var_30->rq_disk->first_minor % 4;
	var_3c = var_30->sector;
	var_40 = 0;
	memset(var_2c.x40[var35], 0, 0x6000);
	memset(var_2c.xc, 0, 16);
	if(!var_30->bio) goto CF0;
	var_44 = var_30->bio;
BDE:
	if(var_44->bi_idx >= var_44->bi_vcnt) goto CC9;
C10:
	var_48 += var_44->bi_io_vec[var_44->bi_idx]->bv_len >> 9;
	if(var_48 > 0x30) goto CF0;
	var_50 = kmap_atomic(var_44->bi_io_vec[var_44->bi_idx]->bv_page, 5);
	if(var_50 & 0xfff) goto ED7;
	var_50 += var_44->bi_io_vec[var_44->bi_idx]->bv_offset;
	memcpy(var_2c.x40[var35] + var_40, var_50, var_44->bi_io_vec[var_44->bi_idx]->bv_len);
	kunmap_atomic(var_44->bi_io_vec[var_44->bi_idx]->bv_page, 5);
	var_40 += var_44->bi_io_vec[var_44->bi_idx]->bv_len;
	if(var_48 == var_30->nr_sectors) goto CF0;
	var_44->bi_idx++;
	if(var_44->bi_idx < var_44->bi_vcnt) goto C10;
CC9:
	if((var_44 = var_44->bi_next)) goto BDE;
CF0:
	var_2c.xc->x0 = var_35;
	var_2c.x10 = var_3c; // lba
	var_2c.x18 = var_30->flags & 1; // 0 - read, 1 - write
	var_2c.x14 = var_48; // sectors

	var_34 = tifm_startio(var_2c.xc, var_2c);


	...

	
}

tifm_startio()
{

}

ReadFM()
{
}


//---------------
readsectors:
blocks card - arg_8 (3)

read:
blocks card - arg_8 (3)
dma addr - arg_c (4)
blocks fm - arg_10 (5)

readfm:
dma addr - arg_8 (3)


startio
blocks card - arg_4->x8 (2)
dma addr - arg_4->x4 (2)
