//FIFO
#include<iomanip>
#include<iostream>
using namespace std;
void discard(int Array[][19],int pagenumber[],int page[],int max);
int FIFO(int pagenumber[],int order);
int main()
{
    int Array[4][19];
    int page[19] = {7,0,1,2,0,3,0,4,2,3,0,3,2,1,2,0,1,7,0};
    int pagenumber[3] = {-1,-1,-1};
    //请求求页面序列
    cout<<"请求页面访问序列为："<<endl;
    for(int i = 0;i < 19;i++){
        Array[3][i] = -1;   
        cout<<setw(3)<<page[i];
    }
    int max = 19;
    discard( Array, pagenumber,page,max);
    cout<<endl;
    cout<<endl;
    //输出
    cout<<"输出结果如下表（-2）代表没有缺页中断！"<<endl;
    int LackPageNumber = 0;
    for(int j = 0; j < 4;j++){
        for(int k = 0; k  < 19;k++){
            cout<<setw(3)<<Array[j][k];
            if(j == 3){
                if(Array[j][k] != -2)
                    LackPageNumber++;
            }
        }
        cout<<endl;
    }
    cout<<"缺页次数："<<LackPageNumber<<endl;
    return 0;
}
void discard(int Array[][19],int pagenumber[],int page[],int n)
{
    for(int tt = 0;tt < n;tt++){
        if(FIFO(pagenumber,page[tt])> -1){
        Array[0][tt] = pagenumber[0];
        Array[1][tt] = pagenumber[1];
        Array[2][tt] = pagenumber[2];
        Array[3][tt] = -2;//表示非缺页
    }
    else{
        Array[0][tt] = page[tt];
        Array[1][tt] = pagenumber[0];
        Array[2][tt] = pagenumber[1];
        Array[3][tt] =-1;
        for(int ii = 2;ii >0;ii--){
            pagenumber[ii] = pagenumber[ii-1];
        }
        pagenumber[0] =page[tt];
    }
    }
}
int FIFO(int pagenumber[],int order)
{
    if(pagenumber[0] == order)
        return 0;
    else if(pagenumber[1] == order)
        return 1;
    else if(pagenumber[2] == order)
        return 2;
    else
        return -1;
}