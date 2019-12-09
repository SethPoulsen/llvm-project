#include <math.h>
#include <stdio.h>
#include <assert.h>

float quadratic(double a, double b, double c) {

    double denom = (2 * a);

    double num = -b + sqrt(pow(b, 2) - (4 * a * c) );

    double x = num / denom;

    return x;
}

int main() {
    for (unsigned long i = 0; i < 100000000L; ++i) {
        double x = quadratic(3, -4, -5);   
        assert(x > 2.10 && x < 2.12 || "Wrong Answer!!!");
    }
    return 0;
}
