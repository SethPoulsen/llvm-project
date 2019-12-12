#include <assert.h>

int arr1[] = { 
     1,  2,  3,  4,  5,  6,  7,  8,  9, 10,
    11, 12, 13, 14, 15, 16, 17, 18, 19, 20,
    21, 22, 23, 24, 25, 26, 27, 28, 29, 30 
};
int arr1_len = 30;

int sum(int arr[], int len) {
    int acc = 0;
    for (int i = 0; i < len; ++i) {
        acc += arr[i];
    }
    return acc;
}


int main() {
    for (unsigned long i = 0; i < 50000000L; ++i) {
        int sum1 = sum(arr1, arr1_len); 
        assert(sum1 == 465 && "Incorrect value for Sum 1");
    }
}