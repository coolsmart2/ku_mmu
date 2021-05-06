#include <stdlib.h>
#define PAGE_SIZE 4					// page 크기 4 bytes
#define PD_MASK 0xc0				// page directory bit mask
#define PD_SHIFT 6					// page directory bit shift
#define PMD_MASK 0x30				// page middle directory bit mask
#define PMD_SHIFT 4					// page middle directory bit shift
#define PT_MASK 0x0c				// page table bit mask
#define PT_SHIFT 2					// page table bit shift
#define PFN_MASK 0xfc				// page frame number bit mask
#define PFN_SHIFT 2					// page frame number bit shift
#define PB_MASK 0x03				// present bit mask
#define SWAP_SPACE_OFFSET 0xfe		// swap space offset bit mask

// page table entry 구조체, 크기 1 byte
struct ku_pte { 
	char entry; 
};
// page 구조체, 크기(=페이지 크기) 4 bytes
struct ku_page { 
	char offset[PAGE_SIZE]; 
};

/*
page control block
context switch를 할 때 pid를 이용해서 pdbr을 찾는 역할
물리메모리 공간에 존재
크기 2 bytes
*/
struct ku_pcb {
	char pid;
	char pdbr;
};
/*
replace proc를 할 때 실행된 페이지를 저장하는 구조체
page table base register와 entry의 index를 저장하여
swap out되었을 때 swap space의 어느 위치를 pt의 entry에
저장하여 다시 swap in 할 때 사용
*/
struct ku_page_data {
	char pid;
	char pfn;
	char ptbr;
	char idx;
};

/********************************************************************************************************/

size_t ku_mmu_p_num;			// physical memory 배열 크기
size_t ku_mmu_s_num;			// swap memory(space) 배열 크기
struct ku_page *ku_mmu_p_mem;	// 물리메모리를 관리하는 동적 배열
struct ku_page *ku_mmu_s_mem;	// 스왑메모리를 관리하는 동적 배열

/********************************************************************************************************/
/*
1. 물리메모리와 스왑메모리의 빈공간을 관리하기 위한 노드
2. 스왑아웃을 하기 위한 페이지를 찾기 위한 replace 정책(선입선출)을 위한 노드
- 빈공간을 관리하기 위한 data로 page frame number만 필요하지만 replace 정책을 위한
큐에는 다른 정보도 필요하기 때문에 다른 노드를 만들지 않고 노드의 구조체 재사용
*/
struct ku_node {
	struct ku_page_data data;
	struct ku_node *next;
};

/*
빈공간 관리와 page replace policy를 위한 큐
size 변수를 두어 빈공간을 여유있게 관리
*/
struct ku_queue {
	struct ku_node *front;
	struct ku_node *rear;
	int size;
};

struct ku_queue ku_mmu_p_free;		// 물리메모리의 빈공간을 관리하는 연결리스트
struct ku_queue ku_mmu_p_alloc;	// 물리메모리에 할당된 페이지를 관리하는 연결리스트
struct ku_queue ku_mmu_s_free;		// 스왑메모리의 빈공간을 관리하는 연결리스트

void ku_init_queue(struct ku_queue *_q) {
	_q->front = _q->rear = NULL;
	_q->size = 0;
}

/*
빈공간 리스트에 빈공간을 삽입해주는 함수
*/
void ku_init_queue_data() {
	ku_init_queue(&ku_mmu_p_free);
	ku_init_queue(&ku_mmu_p_alloc);
	ku_init_queue(&ku_mmu_s_free);
	for (unsigned int i = 0; i < ku_mmu_p_num; i++) {
		struct ku_page_data temp;
		temp.pfn = i;
		ku_enqueue(&ku_mmu_p_free, temp);
	}
	for (unsigned int i = 0; i < ku_mmu_s_num; i++) {
		struct ku_page_data temp;
		temp.pfn = i;
		ku_enqueue(&ku_mmu_s_free, temp);
	}
}

/*
빈공간 리스트가 비었는지 확인하는 결과값을 반환하는 함수
비어있을 경우는 0, 값이 들어있을 때는 1 반환
물리메모리의 빈공간을 관리할 때 사용
*/
int ku_q_empty(struct ku_queue *_q) { return _q->size == 0; }

/*
큐에 노드를 삽입하는 함수
*/
void ku_enqueue(struct ku_queue *_q, struct ku_page_data _data) {
	struct ku_node *temp = (struct ku_node*)malloc(sizeof(struct ku_node));
	temp->data = _data;
	temp->next = NULL;
	if (ku_q_empty(_q)) _q->front = temp;
	else _q->rear->next = temp;
	_q->rear = temp;
	_q->size++;
}
/*
큐에 노드 제거 후 제거된 노드를 반환하는 함수
*/
struct ku_page_data ku_dequeue(struct ku_queue *_q) {
	struct ku_page_data ret;
	struct ku_node *temp;
	temp = _q->front;
	ret = temp->data;
	_q->front = temp->next;
	free(temp);
	_q->size--;
	return ret;
}
/********************************************************************************************************/
/*
물리메모리에 있는 pid의 pcb에 있는 pdbr을 반환하는 함수
물리메모리의 페이지에서 pcd을 저장한 페이지를
offset[0] = -1, offset[1] = pid, offset[2] = pdbr로 설정하여
탐색하여 pcb이고 pid일 때 그 페이지의 오프셋을 확인하여
pdbr을 찾는다.
*/
char ku_get_pdbr(char _pid) {
	for (unsigned int i = 0; i < ku_mmu_p_num; i++)
		if (ku_mmu_p_mem[i].offset[0] == -1 && ku_mmu_p_mem[i].offset[1] == _pid)
			return ku_mmu_p_mem[i].offset[2];
	char pdbr = ku_dequeue(&ku_mmu_p_free).pfn;
	char pcb = ku_dequeue(&ku_mmu_p_free).pfn;
	// printf("pdbr: %d, pcb: %d\n", pdbr, pcb);
	ku_mmu_p_mem[pcb].offset[0] = -1; // pdb 넣은 페이지 표시
	ku_mmu_p_mem[pcb].offset[1] = _pid;
	ku_mmu_p_mem[pcb].offset[2] = pdbr;
	return ku_mmu_p_mem[pcb].offset[2];
}

/*
물리메모리의 페이지의 오프셋이 모두 0인 페이지를 반환하는 함수
(페이지를 초기화할때 사용)
*/
struct ku_page ku_default_page() {
	struct ku_page zero_page;
	for (unsigned int i = 0; i < PAGE_SIZE; i++) zero_page.offset[i] = 0;
	return zero_page;
}

/*
count의 값만큼 메모리에 할당된 페이지를 스왑메모리에 할당하고
할당된 페이지를 초기화하고 스왑아웃된 페이지를 페이지 테이블의 entry에
스왑메모리의 위치를 저장하는 함수
스왑아웃에 성공했을 때 0 반환
실패했을 경우는 -1 반환
*/
int ku_swap_out(unsigned int _count) {
	if (ku_q_empty(&ku_mmu_s_mem)) {
		while ((_count--) > 0) {
			struct ku_page_data src = ku_dequeue(&ku_mmu_p_alloc);
			struct ku_page_data tar = ku_dequeue(&ku_mmu_s_free);
			ku_mmu_s_mem[tar.pfn] = ku_mmu_p_mem[src.pfn];
			ku_mmu_p_mem[src.pfn] = ku_default_page();
			ku_enqueue(&ku_mmu_p_free, src);
			char *swap_space_offset = &ku_mmu_p_mem[src.ptbr].offset[src.idx];
			*swap_space_offset = tar.pfn << 1;
			printf("swap out: p(%d) -> s(%d)\n", src.pfn, tar.pfn);
			// printf("after free size: %d\n", ku_mmu_p_free.size);
		}
		return 0;
	}
	return -1;
}

/*
실행되려는 프로세스 페이지의 pte에 present bit 0이고
나머지 비트들이 0이 아닐때는 페이지는 스왑아웃되어 있는 상태로
스왑메모리에 있는 페이지를 다시 물리메모리로 페이지를 할당하고
할당한 page frame number를 반환하는 함수
*/
char ku_swap_in(char _swap_space_offset) {
	struct ku_page_data tar = ku_dequeue(&ku_mmu_p_free);
	char src_pfn = _swap_space_offset;
	ku_mmu_p_mem[tar.pfn] = ku_mmu_s_mem[src_pfn];
	//printf("swap in: s(%d) -> p(%d)\n", src_pfn, tar.pfn);
	return tar.pfn;
}

/*
물리메모리와 스왑메모리의 모든 페이지를 디폴트페이지(offset이 모두 0)로 초기화하는 함수
*/
void ku_init_mem() {
	for (unsigned int i = 0; i < ku_mmu_p_num; i++) ku_mmu_p_mem[i] = ku_default_page();
	for (unsigned int i = 0; i < ku_mmu_s_num; i++) ku_mmu_s_mem[i] = ku_default_page();
}
/********************************************************************************************************/
/*
물리메모리의 크기 비트와 스왑메모리의 크기 비트를 인자로 가지고
전역변수인 물리메모리와 스왑메모리를 동적할당하고 여러 변수에 대한
초기화를 하는 하고 물리메모리의 주소 시작점을 반환하는 함수
전달받은 물리메모리의 크기가 0보다 작으면 오류로 인지하고 0을 반환한다.
*/
void *ku_mmu_init(unsigned int _mem_size, unsigned int _swap_size) {
	if (_mem_size <= 0) return 0;
	ku_mmu_p_num = 1 << _mem_size;
	ku_mmu_s_num = 1 << _swap_size;
	ku_mmu_p_mem = (struct ku_page*)malloc(ku_mmu_p_num * sizeof(struct ku_page));
	ku_mmu_s_mem = (struct ku_page*)malloc(ku_mmu_s_num * sizeof(struct ku_page));
	ku_init_mem();
	ku_init_queue_data();
	return ku_mmu_p_mem;
}

/*
문맥 교환을 할 때 cpu의 cr3레지스터를 해당 pid의 pdbr로 바꾸는 함수
cr3값이 물리메모리 주소의 시작점(base)보다 크고 같고 끝점(bound)보다 작을 때 0을 반환
물리메모리 주소의 범위를 벗어나면 -1을 반환
*/
int ku_run_proc(char _pid, struct ku_pte **_ku_cr3) {
	*_ku_cr3 = (struct ku_pte*)(ku_mmu_p_mem + ku_get_pdbr(_pid));
	if (*_ku_cr3 < ku_mmu_p_mem || *_ku_cr3 >= ku_mmu_p_mem + ku_mmu_p_num) return -1;
	return 0;
}

/*

*/
int ku_page_fault(char _pid, char _va) {
	char pdbr = ku_get_pdbr(_pid);
	char pd_idx = (_va & PD_MASK) >> PD_SHIFT;
	char pmd_idx = (_va & PMD_MASK) >> PMD_SHIFT;
	char pt_idx = (_va & PT_MASK) >> PT_SHIFT;
	// printf("pd_idx: %d, pmd_idx: %d, pt_idx: %d\n", pd_idx, pmd_idx, pt_idx);
	char pmdbr, ptbr, proc;
	// printf("before free size: %d\n", ku_mmu_p_free.size);
	if ((ku_mmu_p_mem[pdbr].offset[pd_idx] & PB_MASK) == 1) {
		pmdbr = (ku_mmu_p_mem[pdbr].offset[pd_idx] & PFN_MASK) >> PFN_SHIFT;
		if ((ku_mmu_p_mem[pmdbr].offset[pmd_idx] & PB_MASK) == 1) {
			ptbr = (ku_mmu_p_mem[pmdbr].offset[pmd_idx] & PFN_MASK) >> PFN_SHIFT;
			char swap_space_offset = (ku_mmu_p_mem[ptbr].offset[pt_idx] & SWAP_SPACE_OFFSET) >> 1;
			if ((ku_mmu_p_mem[ptbr].offset[pt_idx] & PB_MASK) == 1)
				proc = (ku_mmu_p_mem[ptbr].offset[pt_idx] & PFN_MASK) >> PFN_SHIFT;
			else {
				if (ku_mmu_p_free.size < 2) {
					if (ku_swap_out((1 - ku_mmu_p_free.size) + 2) != 0) return -1;
				}
				if (swap_space_offset != 0) proc = ku_swap_in(swap_space_offset);
				else proc = ku_dequeue(&ku_mmu_p_free).pfn;
			}
		}
		else {
			if (ku_mmu_p_free.size < 3) {
				if (ku_swap_out((2 - ku_mmu_p_free.size) + 2) != 0) return -1;
			}
			ptbr = ku_dequeue(&ku_mmu_p_free).pfn;
			proc = ku_dequeue(&ku_mmu_p_free).pfn;
		}
	}
	else {
		if (ku_mmu_p_free.size < 4) {
			if (ku_swap_out((3 - ku_mmu_p_free.size) + 2) != 0) return -1;
		}
		pmdbr = ku_dequeue(&ku_mmu_p_free).pfn;
		ptbr = ku_dequeue(&ku_mmu_p_free).pfn;
		proc = ku_dequeue(&ku_mmu_p_free).pfn;
	}
	// printf("pdbr: %d, pmdbr: %d, ptbr: %d, proc: %d\n", pdbr, pmdbr, ptbr, proc);
	ku_mmu_p_mem[pdbr].offset[pd_idx] = (pmdbr << 2) + 1;
	ku_mmu_p_mem[pmdbr].offset[pmd_idx] = (ptbr << 2) + 1;
	ku_mmu_p_mem[ptbr].offset[pt_idx] = (proc << 2) + 1;
	// printf("pde: %d, pmde: %d, pte: %d\n", (pmdbr << 2) + 1, (ptbr << 2) + 1, (proc << 2) + 1);
	struct ku_page_data alloc_proc;
	alloc_proc.pid = _pid;
	alloc_proc.pfn = proc;
	alloc_proc.ptbr = ptbr;
	alloc_proc.idx = pt_idx;
	ku_enqueue(&ku_mmu_p_alloc, alloc_proc);
	// printf("alloc size: %d\n", ku_mmu_p_alloc.size);
	return 0;
}