#include <stdio.h>
#include <string.h>

#define max 100
//functuin statement
void menu();
void prin();
void bank();
int safe();

int p, r, R[max], v[max], c[max][max], a[max][max], path[max], vis[max];

int main()
{		
	printf("请输入进程数p\n");
	scanf("%d",&p);
	printf("请输入资源数r\n");
	scanf("%d",&r);
	printf("请分别输入每个资源的总个数r[]\n");
	for(int i=0;i<r;i++) scanf("%d",&R[i]);
	printf("请分别输入每个资源的可用个数v[]\n");
	for(int i=0;i<r;i++) scanf("%d",&v[i]);
	printf("请输入每个进程对资源的最大需求矩阵c[][]\n");
	for(int i=0;i<p;i++)
		for(int j=0;j<r;j++)
			scanf("%d",&c[i][j]);
	printf("请输入每个进程对资源的已分配的矩阵a[][]\n");
	for(int i=0;i<p;i++)
		for(int j=0;j<r;j++)
			scanf("%d",&a[i][j]);
	menu();
}
void menu()
{
	int op;
	printf("请输入操作: 1.请求分配资源 2.显示当前状态 3.退出\n");
	scanf("%d",&op);
	if(op==1) {bank();menu();}
	else if(op==2) {prin();menu();}
	else return;
}

void bank()  // allocate resource
{
	int num;
	int bank[max];
	printf("请输入需要分配的进程\n");
	scanf("%d",&num);
	printf("请分别输入分配的资源数目\n"); 
	for(int i=0;i<r;i++)
		scanf("%d",&bank[i]);
	int flag=1;
	for(int i=0;i<r;i++) {
		if(bank[i]>v[i])
			flag=0;
	}
	if(flag==0) {
		printf("分配已超过已有资源!\n");
		return ;
	}
	flag=1;
	for(int i=0;i<r;i++) {
		if((bank[i]+a[num][i])>c[num][i])
		flag=0;
	 }
	 if(flag==0) {
	 	printf("分配已超过所需最大值!\n");
	 	return ;
	  }

	for(int i=0;i<r;i++) {
		a[num][i]=a[num][i]+bank[i];
		v[i]=v[i]-bank[i];
	}
	if(safe()==1) {
		printf("资源分配成功!");
		printf("安全路径是：");
        for(int i=0;i<p;i++)
			printf("%2d",path[i]); 
        for(int i = 0; i < p; i++) {
		   	int j;
        	for(j = 0; j < r; j++) {
                if(a[i][j] != c[i][j])
                    break;
            }
            if(j == r) {
                for(j = 0; j < r; j++) {
                    v[j] += a[i][j];
                    a[i][j] = 0;
                }
            }
        }
		printf("\n");
    }
    else  {
        printf("该状态不安全资源分配失败!\n");
        for(int i = 0; i < r; i++) {
            a[num][i] -= bank[i]; 
		    v[i] += bank[i];
        }
    }
}

void prin()   //display the state
{
	printf("总需求向量c[][]\n");
	for(int i=0;i<p;i++){ 
		for(int j=0;j<r;j++)
			printf("%2d",c[i][j]);
		printf("\n");
	}
	printf("已分配向量a[][]\n");
	for(int i=0;i<p;i++){
		for(int j=0;j<r;j++)
			printf("%2d",a[i][j]);
		printf("\n");
	}
	printf("仍需向量\n");
	for(int i=0;i<p;i++){
		for(int j=0;j<r;j++)
			printf("%2d",c[i][j]-a[i][j]);
		printf("\n");
	}
	printf("可用资源\n");
	for(int i=0;i<r;i++){
		printf("%2d",v[i]);
	printf("\n");
	}
}

int safe() {    //detect if the state is safe or not
    int curV[max]; 
    for(int i = 0; i < r; i++)
        curV[i] = v[i];
    memset(vis, 0, sizeof(vis));
    int flag = 1;
    for(int i1 = 0; i1 < p; i1++) {
        int i;
        for(i = 0; i < p; i++) {
            if(vis[i] == 1) continue;
            int flagpro = 1;
            for(int j = 0; j < r; j++) {
                if(c[i][j] - a[i][j] > curV[j]) {
                    flagpro = 0; break;
                }
            }
            if(flagpro) {
                path[i1] = i;
                vis[i] = 1;
                for(int k = 0; k < r; k++)
                    curV[k] += a[i][k];
                break;
            }
        }
        if(i == p) {
            flag = 0;
        }
    }
 	if(flag==1)
    return 1;
}

