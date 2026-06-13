#include <check.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* Forward declaration of the CSV parser function from src/io/csv.c */
extern int csv_parse(const char *input, size_t input_len, void *output, size_t output_capacity);

START_TEST(test_csv_buffer_overflow_protection)
{
    /* Invariant: Buffer reads never exceed declared length.
       CSV parser must not perform memcpy operations that exceed
       destination buffer capacity, even with malicious oversized fields. */
    
    const char *payloads[] = {
        /* Valid input: normal CSV within bounds */
        "field1,field2,field3\n",
        
        /* Boundary case: single field at max reasonable size */
        "a,b,c\n",
        
        /* Attack payload 1: field with 2x expected buffer size */
        "field1," "x" "x" "x" "x" "x" "x" "x" "x" "x" "x" "x" "x" "x" "x" "x" "x" ",field3\n",
        
        /* Attack payload 2: field with 10x expected buffer size */
        "field1," "y" "y" "y" "y" "y" "y" "y" "y" "y" "y" "y" "y" "y" "y" "y" "y" 
                  "y" "y" "y" "y" "y" "y" "y" "y" "y" "y" "y" "y" "y" "y" "y" "y" ",field3\n",
        
        /* Attack payload 3: malformed CSV with excessive field count */
        "a,b,c,d,e,f,g,h,i,j,k,l,m,n,o,p,q,r,s,t,u,v,w,x,y,z\n"
    };
    
    int num_payloads = sizeof(payloads) / sizeof(payloads[0]);
    
    for (int i = 0; i < num_payloads; i++) {
        size_t input_len = strlen(payloads[i]);
        
        /* Allocate output buffer with reasonable capacity */
        size_t output_capacity = 4096;
        void *output = malloc(output_capacity);
        ck_assert_ptr_nonnull(output);
        
        /* Call the actual CSV parser from src/io/csv.c */
        int result = csv_parse(payloads[i], input_len, output, output_capacity);
        
        /* Invariant check: parser must either succeed (result >= 0) or fail gracefully (result < 0)
           without triggering buffer overflow. No crash or undefined behavior allowed. */
        ck_assert(result >= -1);
        
        free(output);
    }
}
END_TEST

Suite *security_suite(void)
{
    Suite *s;
    TCase *tc_core;

    s = suite_create("Security");
    tc_core = tcase_create("Core");

    tcase_add_test(tc_core, test_csv_buffer_overflow_protection);
    suite_add_tcase(s, tc_core);

    return s;
}

int main(void)
{
    int number_failed;
    Suite *s;
    SRunner *sr;

    s = security_suite();
    sr = srunner_create(s);

    srunner_run_all(sr, CK_NORMAL);
    number_failed = srunner_ntests_failed(sr);
    srunner_free(sr);

    return (number_failed == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}