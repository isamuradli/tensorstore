We have done the comprehensive review of our custome remote_dram kvstore backend. You can refer to the @/home/isa-grc/tensorstore/notes/summary_of_remote_dram/COMPREHENSIVE_REMOTE_DRAM_ANALYSIS.md to read about it 

After getting the overview about it, read the /home/isa-grc/tensorstore/notes/summary_of_remote_dram/PRINCIPAL_ENGINEER_REVIEW_SUMMARY.md to understand the high level challenges and issue it comprises of. 

Read /home/isa-grc/tensorstore/notes/summary_of_remote_dram/key_notes_to_consider/TENSORSTORE_COMPLIANCE_ANALYSIS.md to get the analysis of our custom remote_dram follow native tensorstore development practices. You have to note that our kvstore driver is intended to transfer data from DRAM to DRAM, compared to the gcs, S3 which is persistent storage kvstore drivers 

ALso read /home/isa-grc/tensorstore/notes/summary_of_remote_dram/bottlenecks/UCX_DATA_TRANSFER_BOTTLENECKS.md to get to know existing bottlenecks our development has and its proposed solution for the fix. 

After reading all of this, create the plan and that will address the investigated issues. Do it iteratively and place the plan for the AI agent into the folder @notes/planForFixingPerformance 
You have to keep in mind that client and server nodes are not sharing DRAM, they have each seperate DRAM. 

Ask questions if necessary or you are uncertain 